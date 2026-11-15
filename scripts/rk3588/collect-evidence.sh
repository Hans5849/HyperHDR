#!/usr/bin/env bash
# Collect a read-only evidence bundle for RK3588 HDMI capture validation
# (issues #8, #10, #11, #13). Runs on the board itself.
#
#   scripts/rk3588/collect-evidence.sh [--device /dev/videoN] [--since 30min]
#                                      [--stream N] [--expbuf] [--out DIR]
#
# Default mode only queries: versions, packages, services, every video node's
# capabilities, the HDMI receiver's formats, timings and controls, and recent
# HyperHDR and kernel logs. It does not stop services, change packages,
# change device settings or start streaming, so it is safe while HyperHDR runs.
#
# Opt-in probes that touch the receiver (stop HyperHDR first, or they report
# the node as busy):
#   --stream N   v4l2-ctl MMAP stream of N frames to /dev/null (kernel-level
#                capture only; not HyperHDR or WLED evidence)
#   --expbuf     REQBUFS + VIDIOC_EXPBUF + release, never streams (#13)
#
# Output: DIR/rk3588-evidence-<UTC timestamp>.tar.gz and a printed summary.
# IPv4/IPv6/MAC addresses, the hostname and the current user name are
# redacted from collected text. The HyperHDR database and settings are never
# read. Review the archive before sharing it anyway.
set -u

DEVICE=""; SINCE="30min"; STREAM=0; EXPBUF=0; OUT="."
while [ $# -gt 0 ]; do
	case "$1" in
		--device) DEVICE=$2; shift 2;;
		--since) SINCE=$2; shift 2;;
		--stream) STREAM=$2; shift 2;;
		--expbuf) EXPBUF=1; shift;;
		--out) OUT=$2; shift 2;;
		-h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
		*) echo "unknown option: $1 (see --help)" >&2; exit 2;;
	esac
done
case "$STREAM" in ''|*[!0-9]*) echo "--stream needs a frame count" >&2; exit 2;; esac

HERE=$(cd "$(dirname "$0")" && pwd)
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
WORK=$(mktemp -d)
BUNDLE="$WORK/rk3588-evidence-$STAMP"
mkdir -p "$BUNDLE"
trap 'rm -rf "$WORK"' EXIT

HOST=$(hostname 2>/dev/null || true)
ME=$(id -un 2>/dev/null || true); [ "$ME" = root ] && ME=""   # "root" is not identifying
redact() {
	sed -E \
		-e 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ipv4>/g' \
		-e 's/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/<mac>/g' \
		-e 's/\b([0-9A-Fa-f]{1,4}:){4,7}[0-9A-Fa-f]{1,4}\b/<ipv6>/g' \
		-e 's/\b[0-9A-Fa-f]{1,4}(:[0-9A-Fa-f]{1,4})*::([0-9A-Fa-f]{1,4}(:[0-9A-Fa-f]{1,4})*)?\b/<ipv6>/g' \
		${HOST:+-e "s/\\b${HOST}\\b/<host>/g"} \
		${ME:+-e "s/\\b${ME}\\b/<user>/g"}
}
have() { command -v "$1" >/dev/null 2>&1; }

# capture <file> <command...>: run with a timeout, keep stdout+stderr and rc.
capture() {
	local file="$BUNDLE/$1"; shift
	mkdir -p "$(dirname "$file")"
	{
		echo "\$ $*"
		if have "$1"; then timeout 30 "$@" 2>&1; echo "[exit $?]"; else echo "[not installed: $1]"; fi
	} | redact >"$file"
}

echo "Collecting into $BUNDLE ..."

# --- system --------------------------------------------------------------
capture system/date.txt date -u
capture system/uname.txt uname -a
capture system/os-release.txt cat /etc/os-release
[ -r /etc/armbian-release ] && capture system/armbian-release.txt cat /etc/armbian-release
[ -r /proc/device-tree/model ] && { tr -d '\0' </proc/device-tree/model; echo; } >"$BUNDLE/system/model.txt"
capture system/memory.txt free -m
capture system/uptime.txt uptime
{
	for p in /sys/devices/system/cpu/cpufreq/policy*; do
		[ -d "$p" ] || continue
		echo "$(basename "$p") cpus=$(cat "$p/related_cpus" 2>/dev/null) governor=$(cat "$p/scaling_governor" 2>/dev/null) cur=$(cat "$p/scaling_cur_freq" 2>/dev/null) max=$(cat "$p/scaling_max_freq" 2>/dev/null)"
	done
	for z in /sys/class/thermal/thermal_zone*; do
		[ -r "$z/temp" ] && echo "$(cat "$z/type" 2>/dev/null) $(cat "$z/temp") m°C"
	done
} >"$BUNDLE/system/cpufreq-thermal.txt" 2>&1
[ -d "$HERE/../../.git" ] && capture system/source-tree.txt git -C "$HERE/../.." log -1 --format='%H %cI %s'

# --- package and service -------------------------------------------------
capture hyperhdr/dpkg.txt dpkg-query -W -f '${Package} ${Version} ${Architecture} ${Status}\n' hyperhdr
BIN=$(command -v hyperhdr 2>/dev/null || true)
if [ -n "$BIN" ]; then
	REAL=$(readlink -f "$BIN")
	capture hyperhdr/binary-sha256.txt sha256sum "$REAL"
	# --version exits before the daemon starts (tests/cli/hyperhdr-cli-smoke.sh).
	capture hyperhdr/version.txt "$BIN" --version
fi
capture hyperhdr/services.txt systemctl list-units --all --no-pager 'hyperhdr*'
capture hyperhdr/processes.txt ps -o pid,user,etimes,rss,pcpu,args -C hyperhdr

# --- every video node ------------------------------------------------------
capture v4l2/list-devices.txt v4l2-ctl --list-devices
# RK3588_EVIDENCE_NODES overrides the node list (tests/rk3588 uses it with a stub v4l2-ctl).
NODES=${RK3588_EVIDENCE_NODES:-$(ls /dev/video* 2>/dev/null | sort -V)}
for n in $NODES; do
	capture "v4l2/nodes/$(basename "$n").txt" v4l2-ctl -d "$n" --info
done

# --- receiver --------------------------------------------------------------
RX="$DEVICE"
if [ -z "$RX" ] && have v4l2-ctl; then
	# Match by driver name as reported by the node, never by number.
	for n in $NODES; do
		if v4l2-ctl -d "$n" --info 2>/dev/null | grep -Eq 'Driver name *: *(snps_hdmirx|rk_hdmirx|.*hdmirx.*)'; then RX=$n; break; fi
	done
fi
echo "${RX:-<not found>}" >"$BUNDLE/v4l2/receiver-node.txt"
if [ -n "$RX" ]; then
	R=v4l2/receiver
	capture $R/all.txt v4l2-ctl -d "$RX" --all
	capture $R/formats.txt v4l2-ctl -d "$RX" --list-formats-ext
	capture $R/fmt.txt v4l2-ctl -d "$RX" --get-fmt-video
	capture $R/query-dv-timings.txt v4l2-ctl -d "$RX" --query-dv-timings
	capture $R/get-dv-timings.txt v4l2-ctl -d "$RX" --get-dv-timings
	capture $R/controls.txt v4l2-ctl -d "$RX" --list-ctrls-menus
	capture $R/holders.txt fuser -v "$RX"
	if [ "$STREAM" -gt 0 ]; then
		capture $R/stream.txt v4l2-ctl -d "$RX" --stream-mmap=4 --stream-count="$STREAM" --stream-to=/dev/null --verbose
	fi
	if [ "$EXPBUF" -eq 1 ]; then
		if have cc; then
			cc -O2 -o "$WORK/expbuf-probe" "$HERE/v4l2-expbuf-probe.c" 2>"$BUNDLE/$R/expbuf-build.txt" &&
				capture $R/expbuf.txt "$WORK/expbuf-probe" "$RX"
		else
			echo "[cc not installed]" >"$BUNDLE/$R/expbuf.txt"
		fi
	fi
fi

# --- logs ------------------------------------------------------------------
capture logs/hyperhdr-journal.txt journalctl --no-pager -o short-iso --since "-$SINCE" -u 'hyperhdr*'
{
	echo "\$ journalctl -k --since -$SINCE | grep -Ei 'hdmi|v4l2|video|rga|rkvdec|hantro'"
	have journalctl && journalctl -k --no-pager -o short-iso --since "-$SINCE" 2>&1 | grep -Ei 'hdmi|v4l2|video|rga|rkvdec|hantro'
} | redact >"$BUNDLE/logs/kernel-video.txt"

# --- summary ---------------------------------------------------------------
val() { grep -m1 -E "$2" "$BUNDLE/$1" 2>/dev/null | sed -E "s|.*$2 *||" | tr -s ' \t' '  '; }
line() { grep -m1 -E "$2" "$BUNDLE/$1" 2>/dev/null | tr -s ' \t' '  ' | sed -E 's/^ //'; }
{
	echo "RK3588 evidence $STAMP"
	echo "model:        $(cat "$BUNDLE/system/model.txt" 2>/dev/null)"
	echo "os:           $(val system/os-release.txt 'PRETTY_NAME=' | tr -d '"')"
	echo "armbian:      $(val system/armbian-release.txt 'VERSION=')"
	echo "kernel:       $(uname -r)"
	echo "package:      $(sed -n 2p "$BUNDLE/hyperhdr/dpkg.txt" 2>/dev/null)"
	echo "version:      $(val hyperhdr/version.txt 'Version *:')"
	echo "service:      $(grep -m1 -E 'hyperhdr@' "$BUNDLE/hyperhdr/services.txt" 2>/dev/null | tr -s ' ')"
	echo "receiver:     ${RX:-not found}"
	if [ -n "$RX" ]; then
		echo "driver/bus:   $(val v4l2/receiver/all.txt 'Driver name *:') / $(val v4l2/receiver/all.txt 'Bus info *:')"
		echo "power_present:$(val v4l2/receiver/controls.txt 'power_present.*value=' | awk '{print " "$1}')"
		echo "rgb range:    $(line v4l2/receiver/controls.txt 'rgb.*range|quantization_range')"
		echo "dv timings:   $(sed -n 2p "$BUNDLE/v4l2/receiver/query-dv-timings.txt" 2>/dev/null)"
		echo "format:       $(val v4l2/receiver/fmt.txt 'Width/Height *:') $(val v4l2/receiver/fmt.txt 'Pixel Format *:') planes=$(val v4l2/receiver/fmt.txt 'Number of planes *:')"
		echo "quantization: $(val v4l2/receiver/fmt.txt 'Quantization *:')"
		echo "holders:      $(tail -n +2 "$BUNDLE/v4l2/receiver/holders.txt" 2>/dev/null | grep -v '^\[exit' | tr -s ' \n' ' ')"
		[ "$EXPBUF" -eq 1 ] && echo "expbuf:       $(grep -E '^(open|querycap|reqbufs|expbuf|hint)=' "$BUNDLE/v4l2/receiver/expbuf.txt" 2>/dev/null | tr '\n' ' ')"
		[ "$STREAM" -gt 0 ] && echo "stream:       $(grep -cE '^cap dqbuf' "$BUNDLE/v4l2/receiver/stream.txt" 2>/dev/null) dqbuf lines (see stream.txt)"
	fi
	echo "video nodes:  $(echo $NODES | wc -w)"
	J="$BUNDLE/logs/hyperhdr-journal.txt"
	if grep -q '^\[exit 0\]' "$J"; then
		echo "log lines:    $(grep -cvE '^(\$ |\[exit|-- No entries --)' "$J") HyperHDR, since -$SINCE"
	else
		echo "log lines:    unavailable (see logs/hyperhdr-journal.txt)"
	fi
	echo
	echo "Kernel-level results (v4l2-ctl) are not HyperHDR preview or WLED evidence."
} >"$BUNDLE/SUMMARY.txt"

mkdir -p "$OUT"
ARCHIVE="$(cd "$OUT" && pwd)/rk3588-evidence-$STAMP.tar.gz"
tar -C "$WORK" -czf "$ARCHIVE" "rk3588-evidence-$STAMP"
cat "$BUNDLE/SUMMARY.txt"
echo
echo "Archive: $ARCHIVE"
echo "Review it before sharing; addresses, hostname and user name are redacted."
