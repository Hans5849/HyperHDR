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
#                       opens the receiver (EXPBUF); the preflight never stops it.
#                       A container can't see host processes, so there it also needs
#                       --host-idle-attested
#   --host-idle-attested  the operator confirms the host's HyperHDR service is stopped
#                       (only meaningful in a container; the workflow input
#                       host_idle_confirmed)
#   --env FILE          append A55_CPU=, A76_CPU=, RECEIVER= for later steps
#   --metadata FILE     write run metadata JSON (board, OS, firmware, toolchain)
#   --inputs JSON       workflow inputs to embed in the metadata
#
# Runs natively or inside the Docker runner container (docs/rk3588-board-runner.md):
#   - the device tree is read from /proc/device-tree, or /sys/firmware/devicetree/base
#     when only that is visible; the container needs the host's tree mounted read-only
#   - /proc/cpuinfo lists all host CPUs even inside a container, so the CPUs
#     taskset may really use come from this process's Cpus_allowed_list
#   - a container with a full capability bounding set (--privileged) is refused
#   - the receiver is found in sysfs by name or driver (stream_hdmirx / snps_hdmirx),
#     never by number, and the matching /dev node must exist and be accessible
#
# Read-only. Exit 0 = all requirements hold, 1 = a requirement failed.
set -u
REQ_RX=0; EXCL=0; ATTEST=0; ENVF=""; META=""; INPUTS="{}"
while [ $# -gt 0 ]; do
	case "$1" in
		--require-receiver) REQ_RX=1; shift;;
		--exclusive) EXCL=1; shift;;
		--host-idle-attested) ATTEST=1; shift;;
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

# --- where are we running --------------------------------------------------------
IN_CONTAINER=0
if [ -f "$R/.dockerenv" ] || [ -f "$R/run/.containerenv" ] || grep -qE 'docker|containerd|kubepods|libpod' "$R/proc/1/cgroup" 2>/dev/null; then IN_CONTAINER=1; fi
[ "$IN_CONTAINER" -eq 1 ] && note "running inside a container"

# --- board identity -----------------------------------------------------------
DT="$R/proc/device-tree"
[ -r "$DT/model" ] || DT="$R/sys/firmware/devicetree/base"
MODEL=$(tr -d '\0' <"$DT/model" 2>/dev/null)
COMPAT=$(tr '\0' ' ' <"$DT/compatible" 2>/dev/null)
if [ -z "$MODEL" ]; then bad "no device-tree model; not an Orange Pi 5 Plus (or the device tree is hidden from the runner; a container needs the host's /sys/firmware/devicetree/base mounted read-only at /proc/device-tree)"
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

# cpuinfo shows every host CPU; taskset can only use the ones this process is allowed.
expand() { local part a b; for part in ${1//,/ }; do case "$part" in *-*) a=${part%-*}; b=${part#*-}; seq "$a" "$b";; *) echo "$part";; esac; done | tr '\n' ' '; }
ALLOWED_LIST=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' "$R/proc/self/status" 2>/dev/null)
A55_CPU=""; A76_CPU=""
if printf '%s' "$ALLOWED_LIST" | grep -qE '^[0-9]+([-,][0-9]+)*$'; then
	ALLOWED=" $(expand "$ALLOWED_LIST")"
	note "CPUs this process may use: $ALLOWED_LIST"
	for c in $A55; do case "$ALLOWED" in *" $c "*) A55_CPU=$c; break;; esac; done
	for c in $A76; do case "$ALLOWED" in *" $c "*) A76_CPU=$c; break;; esac; done
	[ "$N55" -eq 4 ] && [ -z "$A55_CPU" ] && bad "none of the Cortex-A55 CPUs ($A55 ) is in this runner's allowed CPU set ($ALLOWED_LIST); fix the container's --cpuset-cpus"
	[ "$N76" -eq 4 ] && [ -z "$A76_CPU" ] && bad "none of the Cortex-A76 CPUs ($A76 ) is in this runner's allowed CPU set ($ALLOWED_LIST); fix the container's --cpuset-cpus"
else
	ALLOWED_LIST=""
	bad "cannot read the allowed CPU list (Cpus_allowed_list in /proc/self/status); refusing to pin a benchmark blindly"
fi
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
for sock in "$R/var/run/docker.sock" "$R/run/docker.sock"; do
	[ -S "$sock" ] && [ -w "$sock" ] && bad "runner can write the Docker socket (equivalent to root)"
done
if [ "$IN_CONTAINER" -eq 1 ]; then
	# --privileged leaves the capability bounding set full (CAP_SYS_ADMIN = bit 21).
	CAPBND=$(sed -n 's/^CapBnd:[[:space:]]*//p' "$R/proc/self/status" 2>/dev/null)
	if ! printf '%s' "$CAPBND" | grep -qE '^[0-9a-fA-F]+$'; then bad "cannot read the container capability set (CapBnd); cannot confirm it is unprivileged"
	elif [ $(( 0x$CAPBND >> 21 & 1 )) -eq 1 ]; then bad "container has CAP_SYS_ADMIN in its bounding set (privileged mode?); run it without --privileged"
	else ok "container is not privileged (CapBnd $CAPBND)"; fi
fi

# --- receiver --------------------------------------------------------------------
# Found by sysfs name or driver, never by number: the node may be /dev/video1 on
# one boot and /dev/video3 on the next. The V4L2 name is e.g. stream_hdmirx and the
# driver snps_hdmirx; both contain "hdmirx".
RX=""; RX_DRV=""; RX_SEEN=""
for n in $(ls "$R"/sys/class/video4linux 2>/dev/null | sort -V); do
	name=$(cat "$R/sys/class/video4linux/$n/name" 2>/dev/null)
	drv=$(basename "$(readlink -f "$R/sys/class/video4linux/$n/device/driver" 2>/dev/null)" 2>/dev/null)
	case "$name $drv" in
		*hdmirx*)
			[ -z "$RX_SEEN" ] && { RX_SEEN=/dev/$n; RX_DRV=$drv; }
			if [ -r "$R/dev/$n" ] && [ -w "$R/dev/$n" ]; then RX=/dev/$n; RX_DRV=$drv; break; fi;;
	esac
done
[ -z "$RX" ] && RX=$RX_SEEN
if [ -n "$RX" ]; then
	if [ ! -e "$R$RX" ]; then
		if [ "$REQ_RX" -eq 1 ]; then bad "receiver $RX is in /sys but its device node is missing; pass it into the container (--device $RX)"
		else note "receiver $RX is in /sys but its device node is not visible to $ME"; fi
	elif [ -r "$R$RX" ] && [ -w "$R$RX" ]; then ok "receiver $RX accessible (driver ${RX_DRV:-unknown}, group $(stat -c %G "$R$RX" 2>/dev/null))"
	elif [ "$REQ_RX" -eq 1 ]; then bad "receiver $RX not readable/writable by $ME (add to group video, e.g. --group-add video; pass the device into a container runner)"
	else note "receiver $RX not accessible to $ME"; fi
elif [ "$REQ_RX" -eq 1 ]; then bad "no HDMI receiver (*hdmirx*) in /sys/class/video4linux (is /sys visible to the container?)"
else note "no HDMI receiver visible"; fi

# --- exclusive access ------------------------------------------------------------
HH=$(pgrep -x hyperhdr 2>/dev/null | tr '\n' ' ')
if [ -n "$HH" ]; then
	if [ "$EXCL" -eq 1 ]; then bad "hyperhdr is running (pid $HH); this run needs exclusive receiver access. Stop it yourself in an idle window; the workflow never stops it"
	else note "hyperhdr is running (pid $HH); only read-only checks will run"; fi
elif [ "$EXCL" -eq 1 ]; then
	if [ "$IN_CONTAINER" -eq 1 ] && [ "$ATTEST" -ne 1 ]; then
		bad "this runner is in a container and cannot see the host's processes, so it can't tell whether HyperHDR is capturing; stop the host service yourself in an idle window, then rerun with host_idle_confirmed (--host-idle-attested)"
	elif [ "$IN_CONTAINER" -eq 1 ]; then note "container can't see host processes; relying on the operator's host_idle_confirmed for exclusive access"
	else ok "no hyperhdr process; exclusive access available"; fi
fi

# --- outputs ---------------------------------------------------------------------
if [ -n "$ENVF" ]; then printf 'A55_CPU=%s\nA76_CPU=%s\nRECEIVER=%s\n' "$A55_CPU" "$A76_CPU" "$RX" >>"$ENVF"; fi
if [ -n "$META" ]; then
	j() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; printf '"%s"' "$(printf '%s' "$s" | tr -d '\000-\037')"; }
	ver() { "$@" 2>/dev/null | head -1; }
	{
		printf '{"schema":1,"generated":%s,' "$(j "$(date -u +%Y-%m-%dT%H:%M:%SZ)")"
		printf '"git":{"sha":%s,"ref":%s,"run_id":%s,"run_attempt":%s},' "$(j "${GITHUB_SHA:-}")" "$(j "${GITHUB_REF:-}")" "$(j "${GITHUB_RUN_ID:-}")" "$(j "${GITHUB_RUN_ATTEMPT:-}")"
		printf '"inputs":%s,' "$INPUTS"
		printf '"board":{"model":%s,"compatible":%s,"a55":%s,"a76":%s,"receiver":%s,"receiver_driver":%s},' "$(j "$MODEL")" "$(j "$COMPAT")" "$(j "${A55# }")" "$(j "${A76# }")" "$(j "$RX")" "$(j "$RX_DRV")"
		printf '"runner":{"container":%s,"allowed_cpus":%s,"pinned_a55":%s,"pinned_a76":%s},' "$([ "$IN_CONTAINER" -eq 1 ] && echo true || echo false)" "$(j "$ALLOWED_LIST")" "$(j "$A55_CPU")" "$(j "$A76_CPU")"
		printf '"os":{"kernel":%s,"os_release":%s,"armbian":%s,"firmware":%s},' "$(j "$(uname -r)")" \
			"$(j "$(sed -n 's/^PRETTY_NAME=//p' "$R/etc/os-release" 2>/dev/null | tr -d '"')")" \
			"$(j "$(sed -n 's/^VERSION=//p' "$R/etc/armbian-release" 2>/dev/null)")" \
			"$(j "$(tr -d '\0' <"$DT/chosen/u-boot,version" 2>/dev/null)")"
		printf '"toolchain":{"gcc":%s,"cmake":%s,"qt":%s}}\n' "$(j "$(ver g++ --version)")" "$(j "$(ver cmake --version)")" \
			"$(j "$(pkg-config --modversion Qt6Core 2>/dev/null)")"
	} >"$META"
fi

if [ "$FAILED" -ne 0 ]; then echo "preflight failed: nothing was measured"; exit 1; fi
echo "preflight passed"
