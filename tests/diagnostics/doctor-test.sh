#!/usr/bin/env bash
# Hardware-free tests for scripts/diagnostics/hyperhdr-doctor.sh (issue #27).
#
# Every scenario runs the doctor with HYPERHDR_DOCTOR_ROOT pointing at a fixture
# tree and PATH limited to stub commands plus a few coreutils, so the host's
# own systemctl/ss/curl/v4l2-ctl never take part. Stubs log their arguments;
# the test asserts that only read-only operations were requested.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DOCTOR="$ROOT/scripts/diagnostics/hyperhdr-doctor.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
ME=$(id -un)
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok:   $*"; }

# Minimal real tools the doctor needs; nothing that would inspect the host.
mkdir -p "$W/core"
for t in bash sh sed awk grep tr cut head tail sort tee timeout mktemp date stat df readlink uname ls wc \
         basename dirname cat cp mv mkdir rm tar gzip sha256sum env printf true false sleep; do
	p=$(command -v "$t" 2>/dev/null) && ln -sf "$p" "$W/core/$t"
done

stub() {   # stub <dir> <name> <body>
	cat >"$1/$2" <<EOF
#!/bin/bash
echo "$2 \$*" >>"$W/calls.log"
$3
EOF
	chmod +x "$1/$2"
}

# scenario <name>: healthy Orange Pi-like system with the service running.
scenario() {
	S="$W/$1"; rm -rf "$S"; mkdir -p "$S/bin" "$S/root/etc" "$S/root/run/systemd/system" "$S/root/proc" \
		"$S/root/dev" "$S/root/usr/share/hyperhdr/lut" "$S/home/.hyperhdr/db" \
		"$S/root/etc/systemd/system/multi-user.target.wants"
	printf 'PRETTY_NAME="Armbian 26.11.0 trixie"\nID=debian\nVERSION_ID="13"\n' >"$S/root/etc/os-release"
	printf 'MemTotal: 4000000 kB\nMemAvailable: 2500000 kB\n' >"$S/root/proc/meminfo"
	touch "$S/root/dev/video0" "$S/root/dev/video1" "$S/home/.hyperhdr/db/hyperhdr.db"
	head -c 2097152 /dev/zero >"$S/root/usr/share/hyperhdr/lut/lut_lin_tables.3d"
	B="$S/bin"
	stub "$B" systemctl 'case "$1" in
		list-units) echo "hyperhdr@'"$ME"'.service loaded active running HyperHdr";;
		is-active) echo "${SVC_STATE:-active}"; [ "${SVC_STATE:-active}" = active ];;
		is-enabled) echo enabled;;
		show) echo "${NRESTARTS:-0}";;
		status) echo "hyperhdr@'"$ME"'.service - HyperHdr";;
		*) exit 1;; esac'
	stub "$B" dpkg-query 'case "$*" in
		*Status*) echo "install ok installed";; *Architecture*) echo arm64;; *Version*) echo "${PKG_VERSION:-24.0.0~beta.0}";; esac'
	stub "$B" hyperhdr 'printf "HyperHdr\n\tVersion   : 24.0.0-beta.0 (master (abc1234))\n"'
	stub "$B" ldd 'echo "	libQt6Core.so.6 => ${QT_LIB:-/usr/lib/aarch64-linux-gnu/libQt6Core.so.6} (0x0000ffff)"; echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6"'
	stub "$B" sha256sum 'echo "0000  $1"'
	stub "$B" pgrep 'printf "%s\n" ${PIDS:-812}'
	stub "$B" ps 'echo "'"$ME"'"'
	stub "$B" getent 'echo "$2:x:1000:1000::'"$S/home"':/bin/bash"'
	stub "$B" id 'case "$1" in -un) echo "'"$ME"'";; -nG) echo "${GROUPS_OF:-'"$ME"' video dialout}";; *) echo "uid=0";; esac'
	stub "$B" hostname 'echo opi5plus'
	stub "$B" ss 'echo "tcp LISTEN 0 50 0.0.0.0:8090 0.0.0.0:* users:((\"${PORT_OWNER:-hyperhdr}\",pid=812,fd=20))"
		echo "tcp LISTEN 0 50 0.0.0.0:8092 0.0.0.0:* users:((\"hyperhdr\",pid=812,fd=21))"
		echo "tcp LISTEN 0 50 0.0.0.0:19444 0.0.0.0:* users:((\"hyperhdr\",pid=812,fd=22))"
		echo "tcp LISTEN 0 50 0.0.0.0:19400 0.0.0.0:* users:((\"hyperhdr\",pid=812,fd=23))"'
	stub "$B" curl 'case "$*" in *json/info*) echo "{\"ver\":\"0.15.0\",\"leds\":{\"count\":120}}";; *) printf 200;; esac'
	stub "$B" ping 'exit 0'
	stub "$B" journalctl 'printf "%s\n" \
		"07:01:05.123 V4L2:snps_hdmirx : <INFO> no signal: no source detected (power_present=0) on /dev/video1; capture paused, checking every 2000 ms" \
		"07:01:06.000 LEDDEVICE : <WARNING> DDP to 192.168.1.50:4048 from opi5plus failed once" \
		"07:01:07.000 WEBSERVER : <INFO> started"'
	stub "$B" fuser 'echo " 812"'
	ln -sf "$ROOT/tests/rk3588/v4l2-ctl-stub" "$B/v4l2-ctl"
	# Commands that would change state: must never be called.
	for bad in usermod chown chmod apt apt-get dpkg rpm sudo; do stub "$B" "$bad" 'exit 0'; done
	: >"$W/calls.log"
}

run() {   # run <scenario> [doctor args...]; sets RC and OUT
	local s=$1; shift
	local e=(HOME="$W/$s/home" PATH="$W/$s/bin:$W/core" HYPERHDR_DOCTOR_ROOT="$W/$s/root") v
	for v in SVC_STATE NRESTARTS PKG_VERSION QT_LIB PIDS GROUPS_OF PORT_OWNER; do
		[ -n "${!v:-}" ] && e+=("$v=${!v}")
	done
	OUT=$(env -i "${e[@]}" bash "$DOCTOR" --no-color "$@" 2>&1)
	RC=$?
}
has() { printf '%s\n' "$OUT" | grep -qE -- "$2" && pass "$1" || fail "$1: expected /$2/ in:
$(printf '%s\n' "$OUT" | sed 's/^/      /' | head -40)"; }
hasnt() { printf '%s\n' "$OUT" | grep -qE -- "$2" && fail "$1: unexpected /$2/" || pass "$1"; }
rc() { [ "$RC" -eq "$1" ] && pass "$2 (exit $RC)" || fail "$2: exit $RC, expected $1"; }

# --- healthy system -------------------------------------------------------------
scenario healthy; run healthy
rc 0 "healthy system has no FAIL"
has "distribution recognised" '^\[PASS\] os.release: Armbian 26.11.0 trixie \(deb packages\)'
has "package found" '^\[PASS\] pkg.installed: hyperhdr 24.0.0~beta.0 \(arm64, dpkg\)'
has "binary version" '^\[PASS\] bin.version: 24.0.0-beta.0'
has "libraries resolve" '^\[PASS\] bin.libraries'
has "prerelease package version accepted" '^\[PASS\] pkg.version: package 24.0.0~beta.0 matches prerelease 24.0.0-beta.0'
has "service user from unit" "service user $ME from systemd unit"
has "service active" "^\\[PASS\\] svc.active: hyperhdr@$ME.service is active"
has "one process" '^\[PASS\] proc.count: one hyperhdr process'
has "user data owned" "^\\[PASS\\] data.dir: .*/home/.hyperhdr owned by $ME"
has "database present, not read" '^\[PASS\] data.db: .*contents not read'
has "shared LUT found" '^\[PASS\] data.lut: .*/usr/share/hyperhdr/lut/lut_lin_tables.3d \(2 MB\)'
has "web port held by hyperhdr" '^\[PASS\] net.port.8090: web port 8090 held by hyperhdr'
has "web UI answers" '^\[PASS\] net.webui'
has "log warnings counted" '^\[WARN\] log.errors: 1 warning'
has "no-signal log explained" '^\[WARN\] log.capture.no_signal: 1 .no signal:. line.*-> the receiver reports no source'
has "video group" "^\\[PASS\\] grp.video: $ME is in group video"
has "receiver identity" '^\[PASS\] capture.node.video1: snps_hdmirx \(MPLANE\) identity v4l2:snps_hdmirx@platform:fdee0000.hdmi_receiver'
hasnt "codec node not a capture device" 'capture.node.video0'
has "single capture device" '^\[PASS\] capture.autoselect: one capture device'
has "no-signal receiver is a warning, not a failure" '^\[WARN\] capture.signal: /dev/video1: no source detected \(power_present=0\)'
has "range evidence" '^\[PASS\] capture.range: format quantization Default.*receiver RGB range Automatic'
has "holder" '^\[PASS\] capture.holder: /dev/video1 held by pid\(s\) 812'
has "summary line" '^Summary: [0-9]+ pass, [0-9]+ warn, 0 fail'

# Read-only behaviour: only query verbs, never a state-changing command.
if grep -qE '^(usermod|chown|chmod|apt|apt-get|dpkg|rpm|sudo) ' "$W/calls.log"; then fail "state-changing command called: $(grep -E '^(usermod|chown|chmod|apt|apt-get|dpkg|rpm|sudo) ' "$W/calls.log" | head -3)"; else pass "no state-changing commands called"; fi
BADSYS=$(grep '^systemctl ' "$W/calls.log" | grep -vE '^systemctl (list-units|is-active|is-enabled|show|status) ')
[ -z "$BADSYS" ] && pass "systemctl used read-only" || fail "systemctl verbs: $BADSYS"
BADV4L=$(grep '^v4l2-ctl ' "$W/calls.log" | grep -vE -- '--info|--list-ctrls-menus|--query-dv-timings|--get-fmt-video')
[ -z "$BADV4L" ] && pass "v4l2-ctl used read-only (no --set, no streaming)" || fail "v4l2-ctl calls: $BADV4L"
grep -q '^v4l2-ctl' "$W/calls.log" 2>/dev/null || true

# --- failure cases --------------------------------------------------------------
scenario lib; QT_LIB="not found" run lib
rc 1 "missing library fails"
has "missing library named" '^\[FAIL\] bin.libraries: missing: libQt6Core.so.6'

scenario svc; SVC_STATE=failed NRESTARTS=4 run svc
rc 1 "failed service fails"
has "failed unit" "^\\[FAIL\\] svc.active: hyperhdr@$ME.service has failed"
has "restart count" '^\[WARN\] svc.restarts: .* restarted 4 time'

scenario owner; run owner --user hyperhdr-other
rc 1 "wrong ownership fails"
has "ownership mismatch with fix" "^\\[FAIL\\] data.dir: .* owned by $ME, service runs as hyperhdr-other -> sudo chown -R hyperhdr-other:"

scenario port; PORT_OWNER=nginx run port
rc 1 "port taken by another program fails"
has "port owner named" '^\[FAIL\] net.port.8090: web port 8090 held by nginx'

scenario group; GROUPS_OF="$ME" run group
rc 1 "missing video group fails"
has "video group fix" "^\\[FAIL\\] grp.video: $ME is not in group video -> sudo usermod -aG video $ME"
has "dialout only a warning" '^\[WARN\]|^\[PASS\]'

scenario twoproc; PIDS="812 900" run twoproc
rc 1 "two instances fail"
has "two processes" '^\[FAIL\] proc.count: 2 hyperhdr processes'

scenario oldpkg; PKG_VERSION=24.0.0 run oldpkg
has "pre-#20 package version flagged" '^\[WARN\] pkg.version: package declares 24.0.0 for prerelease 24.0.0-beta.0 -> .*--allow-downgrades'
scenario cipkg; PKG_VERSION="24.0.0~trixie~beta0" run cipkg
has "CI standard package name accepted" '^\[PASS\] pkg.version: package 24.0.0~trixie~beta0 matches prerelease'

# --- USB-only and missing tools -------------------------------------------------
scenario usb; rm -f "$W/usb/root/dev/video1"; run usb
has "USB system: no receiver checks" '^\[PASS\] capture.autoselect|^\[WARN\] capture.nodes'
hasnt "no receiver signal check without a receiver" 'capture.signal'

scenario bare; for t in v4l2-ctl ss curl ldd fuser pgrep journalctl sqlite3 dpkg-query systemctl; do rm -f "$W/bare/bin/$t"; done
rm -rf "$W/bare/root/run/systemd"; run bare
rc 0 "missing optional tools never FAIL"
has "v4l2-ctl missing is a skip" '^\[SKIP\] capture.nodes: v4l2-ctl not installed -> install v4l-utils'
has "ss missing is a skip" '^\[SKIP\] net.ports: ss not installed'
has "ldd missing is a skip" '^\[SKIP\] bin.libraries: ldd not installed'
has "pgrep missing is a skip" '^\[SKIP\] proc.count: pgrep not installed'
has "no systemd is a warning" '^\[WARN\] sys.systemd'

# --- options, JSON, archive, redaction --------------------------------------------
scenario opts; run opts --checks capture
hasnt "--checks limits groups" 'os.release|svc.active'
has "--checks capture runs capture" 'grp.video'
run opts --checks bogus; rc 2 "bad --checks is a usage error"
run opts --frobnicate; rc 2 "unknown option is a usage error"
run opts --led-host 192.168.1.60
has "WLED reached when asked" '^\[PASS\] led.wled: WLED 0.15.0 on <ipv4>|^\[PASS\] led.wled: WLED 0.15.0 on 192.168.1.60'
grep -q '^ping ' "$W/calls.log" && pass "led-host pings only when asked" || fail "ping not used for --led-host"
scenario nohost; run nohost
grep -q '^ping \|json/info' "$W/calls.log" && fail "contacted an LED host without --led-host" || pass "no LED host contact by default"

scenario json; run json --json
if printf '%s' "$OUT" | python3 -I -c '
import json,sys
d=json.load(sys.stdin)
assert d["schema"]==1 and d["tool"]=="hyperhdr-doctor"
ids=[c["id"] for c in d["checks"]]
assert len(ids)==len(set(ids)), "duplicate ids"
for c in d["checks"]:
    assert set(c)=={"id","group","status","finding","hint","evidence"}, c
    assert c["status"] in ("PASS","WARN","FAIL","SKIP") and c["group"] in ("install","runtime","capture")
s=d["summary"]; assert sum(s.values())==len(d["checks"])
' 2>"$W/json.err"; then pass "JSON is valid and complete"; else fail "JSON: $(cat "$W/json.err" | tail -2)"; fi
hasnt "JSON has no human summary" '^Summary:'

scenario arch; mkdir -p "$W/arch-out"; run arch --archive "$W/arch-out"
A=$(ls "$W/arch-out"/hyperhdr-doctor-*.tar.gz 2>/dev/null | head -1)
[ -n "$A" ] && pass "archive written" || fail "no archive"
mkdir -p "$W/arch-x" && tar -C "$W/arch-x" -xzf "$A" 2>/dev/null
D=$(ls -d "$W/arch-x"/hyperhdr-doctor-* 2>/dev/null)
[ -f "$D/report.json" ] && [ -f "$D/report.txt" ] && [ -f "$D/evidence/log.journal.txt" ] && pass "archive has report and evidence" || fail "archive contents: $(ls -R "$D" 2>/dev/null | head)"
grep -rqE '192\.168\.1\.50|opi5plus' "$D" && fail "archive leaks address or hostname" || pass "archive redacted"
grep -q '07:01:05.123' "$D/evidence/log.journal.txt" && pass "timestamps survive redaction" || fail "timestamps damaged"

scenario db; stub "$W/db/bin" sqlite3 'echo ok'; run db --db-check
has "--db-check runs quick_check" '^\[PASS\] data.db: .* passes PRAGMA quick_check \(read-only\)'
grep -q '^sqlite3 -readonly .*PRAGMA quick_check' "$W/calls.log" && pass "sqlite3 opened read-only" || fail "sqlite3 call: $(grep sqlite3 "$W/calls.log")"
scenario nodb; stub "$W/nodb/bin" sqlite3 'echo ok'; run nodb
grep -q '^sqlite3' "$W/calls.log" && fail "database opened without --db-check" || pass "database not opened by default"

[ "$FAILS" -eq 0 ] && { echo "all doctor checks passed"; exit 0; }
echo "$FAILS check(s) failed"; exit 1
