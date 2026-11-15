#!/usr/bin/env bash
# Hardware-free tests for scripts/rk3588/board-preflight.sh (issue #28).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PF="$ROOT/scripts/rk3588/board-preflight.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok:   $*"; }

# board <name> <model> <a55 count> <a76 count> <receiver: yes|no>
# Optional environment: ALLOWED (Cpus_allowed_list, default 0-7), CONTAINER=1 (Docker
# runner), CAPBND (capability bounding set), RXNODE/RXNAME/RXDRV (receiver node name,
# V4L2 name and sysfs driver), RXDEV=no (node missing from /dev), DTPATH=sysfs (tree
# only under /sys/firmware/devicetree/base), NOSTATUS=1 (no /proc/self/status).
board() {
	local B="$W/$1"; mkdir -p "$B/root/proc/device-tree/chosen" "$B/root/etc" "$B/root/dev" "$B/root/sys/class/video4linux" "$B/bin"
	local DT="$B/root/proc/device-tree"
	if [ "${DTPATH:-}" = sysfs ]; then DT="$B/root/sys/firmware/devicetree/base"; mkdir -p "$DT/chosen"; fi
	printf '%s\0' "$2" >"$DT/model"
	printf 'xunlong,orangepi-5-plus\0rockchip,rk3588\0' >"$DT/compatible"
	printf 'U-Boot 2024.07-armbian\0' >"$DT/chosen/u-boot,version"
	mkdir -p "$B/root/proc/self"
	if [ -z "${NOSTATUS:-}" ]; then
		printf 'Name:\tbash\nCpus_allowed_list:\t%s\nCapBnd:\t%s\n' "${ALLOWED:-0-7}" "${CAPBND:-000001ffffffffff}" >"$B/root/proc/self/status"
	fi
	[ "${CONTAINER:-0}" = 1 ] && : >"$B/root/.dockerenv"
	printf 'PRETTY_NAME="Armbian 26.11.0 trixie"\n' >"$B/root/etc/os-release"
	printf 'VERSION=26.11.0\n' >"$B/root/etc/armbian-release"
	: >"$B/root/proc/cpuinfo"; local i=0
	for _ in $(seq 1 "$3"); do printf 'processor\t: %d\nCPU part\t: 0xd05\n\n' $i >>"$B/root/proc/cpuinfo"; i=$((i+1)); done
	for _ in $(seq 1 "$4"); do printf 'processor\t: %d\nCPU part\t: 0xd0b\n\n' $i >>"$B/root/proc/cpuinfo"; i=$((i+1)); done
	mkdir -p "$B/root/sys/class/video4linux/video0"; echo rkvdec >"$B/root/sys/class/video4linux/video0/name"
	if [ "$5" = yes ]; then
		local N=${RXNODE:-video1}
		mkdir -p "$B/root/sys/class/video4linux/$N/device" "$B/root/sys/bus/platform/drivers/${RXDRV:-snps_hdmirx}"
		echo "${RXNAME:-snps_hdmirx}" >"$B/root/sys/class/video4linux/$N/name"
		ln -s "$B/root/sys/bus/platform/drivers/${RXDRV:-snps_hdmirx}" "$B/root/sys/class/video4linux/$N/device/driver"
		[ "${RXDEV:-yes}" = yes ] && touch "$B/root/dev/$N"
	fi
	printf '#!/bin/bash\ncase "$1" in -un) echo tester;; -nG) echo "tester video";; -u) echo 1000;; esac\n' >"$B/bin/id"
	printf '#!/bin/bash\n[ -n "${HH_PIDS:-}" ] || exit 1\nprintf "%%s\\n" $HH_PIDS\n' >"$B/bin/pgrep"
	printf '#!/bin/bash\necho "sudo $*" >>"%s/calls"\n[ "${SUDO_OK:-0}" = 1 ]\n' "$W" >"$B/bin/sudo"
	chmod +x "$B/bin/"*
}
run() {   # run <board> [args]
	local B="$W/$1"; shift
	OUT=$(PATH="$B/bin:$PATH" HYPERHDR_PREFLIGHT_ROOT="$B/root" bash "$PF" "$@" 2>&1); RC=$?
}
expect() { [ "$RC" -eq "$1" ] && printf '%s\n' "$OUT" | grep -qE -- "$3" && pass "$2" || fail "$2 (exit $RC): $(printf '%s\n' "$OUT" | grep -E 'FAIL|preflight' | head -3)"; }

board opi "Xunlong Orange Pi 5 Plus" 4 4 yes
run opi --require-receiver --env "$W/env" --metadata "$W/meta.json" --inputs '{"stage":"probe","expbuf":false}'
expect 0 "Orange Pi passes" 'preflight passed'
printf '%s\n' "$OUT" | grep -q 'CPUs: A55 = 0 1 2 3, A76 = 4 5 6 7' && pass "topology mapped" || fail "topology: $OUT"
grep -qx 'A55_CPU=0' "$W/env" && grep -qx 'A76_CPU=4' "$W/env" && grep -qx 'RECEIVER=/dev/video1' "$W/env" && pass "env file for later steps" || fail "env: $(cat "$W/env")"
python3 -I -c '
import json,sys
d=json.load(open(sys.argv[1]))
assert d["board"]["model"]=="Xunlong Orange Pi 5 Plus" and d["board"]["a76"]=="4 5 6 7" and d["board"]["receiver"]=="/dev/video1"
assert d["inputs"]=={"stage":"probe","expbuf":False} and d["os"]["armbian"]=="26.11.0" and d["os"]["firmware"].startswith("U-Boot")
' "$W/meta.json" && pass "metadata JSON" || fail "metadata: $(cat "$W/meta.json")"

board t6 "FriendlyElec NanoPC-T6" 4 4 yes; run t6
expect 1 "another RK3588 board is refused" "model is 'FriendlyElec NanoPC-T6', expected an Orange Pi 5 Plus"

board topo "Xunlong Orange Pi 5 Plus" 8 0 yes; run topo
expect 1 "wrong CPU topology is refused" 'expected 4x Cortex-A55 \+ 4x Cortex-A76, found 8 \+ 0'

board norx "Xunlong Orange Pi 5 Plus" 4 4 no; run norx --require-receiver
expect 1 "missing receiver fails the probe stage" 'no HDMI receiver'
run norx
expect 0 "missing receiver is only a note for the tests stage" 'note:  no HDMI receiver visible'

board busy "Xunlong Orange Pi 5 Plus" 4 4 yes
HH_PIDS=812 run busy --exclusive
expect 1 "running HyperHDR blocks exclusive runs" 'hyperhdr is running \(pid 812 \).*never stops it'
HH_PIDS=812 run busy
expect 0 "running HyperHDR is fine for read-only runs" 'only read-only checks will run'

board sudo "Xunlong Orange Pi 5 Plus" 4 4 yes; SUDO_OK=1 run sudo
expect 1 "passwordless sudo is refused" 'passwordless sudo'
grep -q '^sudo -n true$' "$W/calls" && pass "sudo only probed with -n true" || fail "sudo calls: $(cat "$W/calls")"

board nodt "" 4 4 yes; rm -f "$W/nodt/root/proc/device-tree/model"; run nodt
expect 1 "missing device tree is a failure, not a pass" 'no device-tree model'

# --- Docker runner: orange-pi-5-plus container, /dev/video3 only -------------------------
CONTAINER=1 CAPBND=00000000a80425fb RXNODE=video3 RXNAME=stream_hdmirx RXDRV=snps_hdmirx board dock "Xunlong Orange Pi 5 Plus" 4 4 yes
run dock --require-receiver --env "$W/denv" --metadata "$W/dmeta.json" --inputs '{"stage":"probe","expbuf":false}'
expect 0 "unprivileged Docker runner passes" 'preflight passed'
printf '%s\n' "$OUT" | grep -q 'running inside a container' && pass "container detected" || fail "container: $OUT"
printf '%s\n' "$OUT" | grep -q 'container is not privileged' && pass "unprivileged container confirmed" || fail "caps: $OUT"
printf '%s\n' "$OUT" | grep -q 'receiver /dev/video3 accessible (driver snps_hdmirx' && pass "receiver found by name/driver on /dev/video3" || fail "receiver: $OUT"
grep -qx 'RECEIVER=/dev/video3' "$W/denv" && pass "receiver number is discovered, not assumed" || fail "env: $(cat "$W/denv")"
python3 -I -c '
import json,sys
d=json.load(open(sys.argv[1]))
assert d["runner"]["container"] is True and d["runner"]["allowed_cpus"]=="0-7" and d["runner"]["pinned_a76"]=="4"
assert d["board"]["receiver"]=="/dev/video3" and d["board"]["receiver_driver"]=="snps_hdmirx"
' "$W/dmeta.json" && pass "metadata records container, allowed CPUs and receiver driver" || fail "docker metadata: $(cat "$W/dmeta.json")"

CONTAINER=1 CAPBND=00000000a80425fb board dockx "Xunlong Orange Pi 5 Plus" 4 4 yes
run dockx --require-receiver --exclusive
expect 1 "container can't prove the host is idle, so exclusive access fails closed" 'cannot see the host.s processes'
run dockx --require-receiver --exclusive --host-idle-attested
expect 0 "operator attestation allows exclusive access from a container" 'relying on the operator'
HH_PIDS=812 run dockx --require-receiver --exclusive --host-idle-attested
expect 1 "a visible hyperhdr process still blocks, attested or not" 'hyperhdr is running'
run opi --require-receiver --exclusive
expect 0 "native runner needs no attestation" 'no hyperhdr process; exclusive access available'

CONTAINER=1 CAPBND=000001ffffffffff board privd "Xunlong Orange Pi 5 Plus" 4 4 yes; run privd
expect 1 "privileged container is refused" 'CAP_SYS_ADMIN in its bounding set'
CONTAINER=1 NOSTATUS=1 board nocap "Xunlong Orange Pi 5 Plus" 4 4 yes; run nocap
expect 1 "unreadable capability set is a failure, not a pass" 'cannot read the allowed CPU list|cannot read the container capability set'
board native "Xunlong Orange Pi 5 Plus" 4 4 yes; run native
expect 0 "native runner with a full bounding set is fine" 'preflight passed'

ALLOWED=4-7 board a76only "Xunlong Orange Pi 5 Plus" 4 4 yes; run a76only
expect 1 "container limited to the A76 cluster can't run the A55 benchmark" 'none of the Cortex-A55 CPUs .* allowed CPU set \(4-7\)'
ALLOWED=2-3,6-7 board split "Xunlong Orange Pi 5 Plus" 4 4 yes; run split --env "$W/senv"
expect 0 "partial cpuset passes" 'CPUs this process may use: 2-3,6-7'
grep -qx 'A55_CPU=2' "$W/senv" && grep -qx 'A76_CPU=6' "$W/senv" && pass "pins to allowed CPUs, not the first in cpuinfo" || fail "senv: $(cat "$W/senv")"
NOSTATUS=1 board nostat "Xunlong Orange Pi 5 Plus" 4 4 yes; run nostat
expect 1 "missing allowed-CPU list is a failure" 'cannot read the allowed CPU list'

CONTAINER=1 CAPBND=00000000a80425fb RXNODE=video3 RXNAME=stream_hdmirx RXDEV=no board nonode "Xunlong Orange Pi 5 Plus" 4 4 yes
run nonode --require-receiver
expect 1 "receiver in /sys but not passed into the container fails the probe stage" 'device node is missing; pass it into the container'
run nonode
expect 0 "missing receiver node is a note for the tests stage" 'device node is not visible'

DTPATH=sysfs board dtsys "Xunlong Orange Pi 5 Plus" 4 4 yes; run dtsys
expect 0 "device tree under /sys/firmware/devicetree/base is accepted" 'ok:    model: Xunlong Orange Pi 5 Plus'
CONTAINER=1 CAPBND=00000000a80425fb board dtmiss "Xunlong Orange Pi 5 Plus" 4 4 yes; rm -f "$W/dtmiss/root/proc/device-tree/model"; run dtmiss
expect 1 "hidden device tree in a container is a failure and says what to mount" 'mounted read-only at /proc/device-tree'

if grep -nE 'systemctl +(stop|start|restart)|kill |pkill|usermod|chown|chmod|apt(-get)? ' "$PF" | grep -v '^\s*#'; then fail "preflight contains state-changing commands"; else pass "preflight is read-only"; fi

[ "$FAILS" -eq 0 ] && { echo "all preflight checks passed"; exit 0; }
echo "$FAILS check(s) failed"; exit 1
