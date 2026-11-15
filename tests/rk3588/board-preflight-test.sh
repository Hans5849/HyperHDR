#!/usr/bin/env bash
# Hardware-free tests for scripts/rk3588/board-preflight.sh (issue #28).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PF="$ROOT/scripts/rk3588/board-preflight.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok:   $*"; }

board() {   # board <name> <model> <a55 count> <a76 count> <receiver: yes|no>
	local B="$W/$1"; mkdir -p "$B/root/proc/device-tree/chosen" "$B/root/etc" "$B/root/dev" "$B/root/sys/class/video4linux" "$B/bin"
	printf '%s\0' "$2" >"$B/root/proc/device-tree/model"
	printf 'xunlong,orangepi-5-plus\0rockchip,rk3588\0' >"$B/root/proc/device-tree/compatible"
	printf 'U-Boot 2024.07-armbian\0' >"$B/root/proc/device-tree/chosen/u-boot,version"
	printf 'PRETTY_NAME="Armbian 26.11.0 trixie"\n' >"$B/root/etc/os-release"
	printf 'VERSION=26.11.0\n' >"$B/root/etc/armbian-release"
	: >"$B/root/proc/cpuinfo"; local i=0
	for _ in $(seq 1 "$3"); do printf 'processor\t: %d\nCPU part\t: 0xd05\n\n' $i >>"$B/root/proc/cpuinfo"; i=$((i+1)); done
	for _ in $(seq 1 "$4"); do printf 'processor\t: %d\nCPU part\t: 0xd0b\n\n' $i >>"$B/root/proc/cpuinfo"; i=$((i+1)); done
	mkdir -p "$B/root/sys/class/video4linux/video0"; echo rkvdec >"$B/root/sys/class/video4linux/video0/name"
	if [ "$5" = yes ]; then
		mkdir -p "$B/root/sys/class/video4linux/video1"; echo snps_hdmirx >"$B/root/sys/class/video4linux/video1/name"
		touch "$B/root/dev/video1"
	fi
	printf '#!/bin/bash\ncase "$1" in -un) echo tester;; -nG) echo "tester video";; -u) echo 1000;; esac\n' >"$B/bin/id"
	printf '#!/bin/bash\nprintf "%%s\\n" ${HH_PIDS:-}\n[ -n "${HH_PIDS:-}" ]\n' >"$B/bin/pgrep"
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

if grep -nE 'systemctl +(stop|start|restart)|kill |pkill|usermod|chown|chmod|apt(-get)? ' "$PF" | grep -v '^\s*#'; then fail "preflight contains state-changing commands"; else pass "preflight is read-only"; fi

[ "$FAILS" -eq 0 ] && { echo "all preflight checks passed"; exit 0; }
echo "$FAILS check(s) failed"; exit 1
