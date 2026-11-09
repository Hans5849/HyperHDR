#!/usr/bin/env bash
# Black-box command-line checks for an installed or freshly built hyperhdr binary.
#
#   tests/cli/hyperhdr-cli-smoke.sh [--daemon] [path/to/hyperhdr]
#
# Informational commands (--help, --version) and argument errors must exit
# promptly without starting the daemon or touching the user data directory.
# --daemon additionally starts the daemon briefly with --debug/--verbose and a
# throw-away --userdata directory; it opens the normal network ports, so only
# use it where no other HyperHDR instance is running.
set -u

DAEMON=0
if [ "${1:-}" = "--daemon" ]; then DAEMON=1; shift; fi
BIN="${1:-hyperhdr}"
command -v "$BIN" >/dev/null 2>&1 || { echo "hyperhdr binary not found: $BIN" >&2; exit 2; }

if pgrep -x hyperhdr >/dev/null 2>&1; then
	echo "Another hyperhdr process is running; stop it first (it changes exit codes)." >&2
	exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok:   $*"; }

# run <seconds> <args...>: sets RC, OUT (stdout), ERR (stderr)
run() {
	local limit=$1; shift
	timeout "$limit" "$BIN" "$@" >"$WORK/out" 2>"$WORK/err"
	RC=$?
	OUT=$(cat "$WORK/out"); ERR=$(cat "$WORK/err")
}

no_qt_option_warnings() {
	if grep -q "QCommandLineParser" "$WORK/out" "$WORK/err"; then
		fail "$1: QCommandLineParser warning: $(grep -h QCommandLineParser "$WORK/out" "$WORK/err" | head -3)"
	fi
}

UNUSED="$WORK/must-not-exist"

for help in --help -h; do
	run 20 "$help" -u "$UNUSED"
	[ "$RC" -eq 0 ] && pass "$help exits 0" || fail "$help exit code $RC (124 = still running)"
	grep -q -- "--userdata <userdata>" "$WORK/out" && pass "$help prints usage on stdout" || fail "$help usage missing from stdout"
	no_qt_option_warnings "$help"
	[ ! -e "$UNUSED" ] || fail "$help created the user data directory"
done

run 20 --version -u "$UNUSED"
[ "$RC" -eq 0 ] && pass "--version exits 0" || fail "--version exit code $RC (124 = still running)"
grep -q "Version *:" "$WORK/out" && pass "--version prints version on stdout" || fail "--version output: $OUT"
no_qt_option_warnings "--version"
[ ! -e "$UNUSED" ] && pass "--version does not start the daemon" || fail "--version created the user data directory"

run 20 --userdata
[ "$RC" -ne 0 ] && [ "$RC" -ne 124 ] && pass "--userdata without value exits $RC" || fail "--userdata without value exit code $RC"
grep -q "Missing value" "$WORK/out" "$WORK/err" && pass "--userdata reports missing value" || fail "--userdata error text: $ERR"

run 20 --definitely-not-an-option
[ "$RC" -ne 0 ] && [ "$RC" -ne 124 ] && pass "unknown option exits $RC" || fail "unknown option exit code $RC"

run 20 --debug=1
[ "$RC" -ne 0 ] && [ "$RC" -ne 124 ] && pass "value on a flag is rejected" || fail "--debug=1 exit code $RC"

run 20 --debug --verbose -u "$UNUSED"
[ "$RC" -eq 1 ] && pass "--debug with --verbose exits 1" || fail "--debug --verbose exit code $RC"
grep -q "can't be used together" "$WORK/out" "$WORK/err" && pass "conflicting log options explained" || fail "conflict message missing"
no_qt_option_warnings "--debug --verbose"
[ ! -e "$UNUSED" ] || fail "conflicting options created the user data directory"

if [ "$DAEMON" -eq 1 ]; then
	for flag in --debug --verbose; do
		DATA="$WORK/data${flag}"
		run 15 "$flag" --userdata "$DATA"
		[ "$RC" -eq 124 ] && pass "$flag daemon kept running until stopped" || fail "$flag daemon exited early with $RC: $(tail -5 "$WORK/out")"
		[ -f "$DATA/db/hyperhdr.db" ] && pass "$flag used --userdata $DATA" || fail "$flag did not create $DATA/db/hyperhdr.db"
		no_qt_option_warnings "$flag daemon"
	done
fi

if [ "$FAILS" -ne 0 ]; then
	echo "$FAILS check(s) failed"
	exit 1
fi
echo "all command-line checks passed"
