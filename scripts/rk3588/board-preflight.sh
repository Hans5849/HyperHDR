#!/usr/bin/env bash
# Preflight for the hardware-in-the-loop board workflow (issue #28).
#
#   scripts/rk3588/board-preflight.sh [--require-receiver] [--exclusive]
#                                     [--env FILE] [--metadata FILE] [--inputs JSON]
#
# Verifies that this really is an Orange Pi 5 Plus (RK3588) with the expected
# 4x Cortex-A55 + 4x Cortex-A76 topology before anything is measured, and
# reports the runner's privileges. Nothing absent is treated as a pass: each
# requirement either holds or the preflight fails with the reason.
#
#   --require-receiver  an HDMI receiver (driver *hdmirx*) must be present and
#                       readable by this user (probe stage)
#   --exclusive         no hyperhdr process may run, because the next step
#                       opens the receiver (EXPBUF); the preflight never stops it
#   --env FILE          append A55_CPU=, A76_CPU=, RECEIVER= for later steps
#   --metadata FILE     write run metadata JSON (board, OS, firmware, toolchain)
#   --inputs JSON       workflow inputs to embed in the metadata
#
# Read-only. Exit 0 = all requirements hold, 1 = a requirement failed.
set -u
REQ_RX=0; EXCL=0; ENVF=""; META=""; INPUTS="{}"
while [ $# -gt 0 ]; do
	case "$1" in
		--require-receiver) REQ_RX=1; shift;;
		--exclusive) EXCL=1; shift;;
		--env) ENVF=$2; shift 2;;
		--metadata) META=$2; shift 2;;
		--inputs) INPUTS=$2; shift 2;;
		*) echo "unknown option: $1" >&2; exit 2;;
	esac
done
R=${HYPERHDR_PREFLIGHT_ROOT:-}
FAILED=0
ok() { echo "ok:    $*"; }
bad() { echo "FAIL:  $*"; FAILED=1; }
note() { echo "note:  $*"; }

# --- board identity -----------------------------------------------------------
MODEL=$(tr -d '\0' <"$R/proc/device-tree/model" 2>/dev/null)
COMPAT=$(tr '\0' ' ' <"$R/proc/device-tree/compatible" 2>/dev/null)
if [ -z "$MODEL" ]; then bad "no device-tree model; not an Orange Pi 5 Plus (or /proc/device-tree hidden from the runner)"
elif printf '%s' "$MODEL" | grep -qi 'orange pi 5 plus'; then ok "model: $MODEL"
else bad "model is '$MODEL', expected an Orange Pi 5 Plus; check the runner labels"; fi
case " $COMPAT " in *" rockchip,rk3588 "*) ok "SoC: rockchip,rk3588";; *) bad "compatible '$COMPAT' does not include rockchip,rk3588";; esac

# --- CPU topology: CPU part 0xd05 = Cortex-A55, 0xd0b = Cortex-A76 -----------------
A55=""; A76=""
while read -r cpu part; do
	case "$part" in 0xd05) A55="$A55 $cpu";; 0xd0b) A76="$A76 $cpu";; esac
done < <(awk -F': *' '/^processor/ {p=$2} /^CPU part/ {print p, $2}' "$R/proc/cpuinfo" 2>/dev/null)
N55=$(echo $A55 | wc -w); N76=$(echo $A76 | wc -w)
if [ "$N55" -eq 4 ] && [ "$N76" -eq 4 ]; then ok "CPUs: A55 =$A55, A76 =$A76"
else bad "expected 4x Cortex-A55 + 4x Cortex-A76, found $N55 + $N76 (all CPUs online? cpuinfo visible?)"; fi
A55_CPU=$(echo $A55 | awk '{print $1}'); A76_CPU=$(echo $A76 | awk '{print $1}')
for c in $A55_CPU $A76_CPU; do
	g=$(cat "$R/sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor" 2>/dev/null)
	[ -n "$g" ] && note "cpu$c governor $g, $(cat "$R/sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq" 2>/dev/null) kHz"
done

# --- privileges: report, never change --------------------------------------------
ME=$(id -un); GR=" $(id -nG) "
note "runner user $ME, groups:$GR"
[ "$(id -u)" -eq 0 ] && bad "runner runs as root; use a dedicated unprivileged account (docs/rk3588-board-runner.md)"
if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
	bad "runner has passwordless sudo; the policy is no sudo for the runner account"
else ok "no passwordless sudo"; fi
if [ -S "$R/var/run/docker.sock" ] && [ -w "$R/var/run/docker.sock" ]; then bad "runner can write the Docker socket (equivalent to root)"; fi

# --- receiver --------------------------------------------------------------------
RX=""
for n in $(ls "$R"/sys/class/video4linux 2>/dev/null); do
	name=$(cat "$R/sys/class/video4linux/$n/name" 2>/dev/null)
	drv=$(basename "$(readlink -f "$R/sys/class/video4linux/$n/device/driver" 2>/dev/null)" 2>/dev/null)
	case "$name $drv" in *hdmirx*) RX=/dev/$n; break;; esac
done
if [ -n "$RX" ]; then
	if [ -r "$R$RX" ] && [ -w "$R$RX" ]; then ok "receiver $RX accessible"
	elif [ "$REQ_RX" -eq 1 ]; then bad "receiver $RX not readable/writable by $ME (add to group video; pass the device into a container runner)"
	else note "receiver $RX not accessible to $ME"; fi
elif [ "$REQ_RX" -eq 1 ]; then bad "no HDMI receiver (*hdmirx*) in /sys/class/video4linux"
else note "no HDMI receiver visible"; fi

# --- exclusive access ------------------------------------------------------------
HH=$(pgrep -x hyperhdr 2>/dev/null | tr '\n' ' ')
if [ -n "$HH" ]; then
	if [ "$EXCL" -eq 1 ]; then bad "hyperhdr is running (pid $HH); this run needs exclusive receiver access. Stop it yourself in an idle window; the workflow never stops it"
	else note "hyperhdr is running (pid $HH); only read-only checks will run"; fi
elif [ "$EXCL" -eq 1 ]; then ok "no hyperhdr process; exclusive access available"; fi

# --- outputs ---------------------------------------------------------------------
if [ -n "$ENVF" ]; then printf 'A55_CPU=%s\nA76_CPU=%s\nRECEIVER=%s\n' "$A55_CPU" "$A76_CPU" "$RX" >>"$ENVF"; fi
if [ -n "$META" ]; then
	j() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; printf '"%s"' "$(printf '%s' "$s" | tr -d '\000-\037')"; }
	ver() { "$@" 2>/dev/null | head -1; }
	{
		printf '{"schema":1,"generated":%s,' "$(j "$(date -u +%Y-%m-%dT%H:%M:%SZ)")"
		printf '"git":{"sha":%s,"ref":%s,"run_id":%s,"run_attempt":%s},' "$(j "${GITHUB_SHA:-}")" "$(j "${GITHUB_REF:-}")" "$(j "${GITHUB_RUN_ID:-}")" "$(j "${GITHUB_RUN_ATTEMPT:-}")"
		printf '"inputs":%s,' "$INPUTS"
		printf '"board":{"model":%s,"compatible":%s,"a55":%s,"a76":%s,"receiver":%s},' "$(j "$MODEL")" "$(j "$COMPAT")" "$(j "${A55# }")" "$(j "${A76# }")" "$(j "$RX")"
		printf '"os":{"kernel":%s,"os_release":%s,"armbian":%s,"firmware":%s},' "$(j "$(uname -r)")" \
			"$(j "$(sed -n 's/^PRETTY_NAME=//p' "$R/etc/os-release" 2>/dev/null | tr -d '"')")" \
			"$(j "$(sed -n 's/^VERSION=//p' "$R/etc/armbian-release" 2>/dev/null)")" \
			"$(j "$(tr -d '\0' <"$R/proc/device-tree/chosen/u-boot,version" 2>/dev/null)")"
		printf '"toolchain":{"gcc":%s,"cmake":%s,"qt":%s}}\n' "$(j "$(ver g++ --version)")" "$(j "$(ver cmake --version)")" \
			"$(j "$(pkg-config --modversion Qt6Core 2>/dev/null)")"
	} >"$META"
fi

if [ "$FAILED" -ne 0 ]; then echo "preflight failed: nothing was measured"; exit 1; fi
echo "preflight passed"
