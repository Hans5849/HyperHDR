#!/usr/bin/env bash
# Hardware-free test of scripts/rk3588/collect-evidence.sh: runs it against a
# stub v4l2-ctl/journalctl describing the Orange Pi 5 Plus receiver with no
# HDMI source, then checks the summary, redaction and read-only guarantees.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRIPT="$ROOT/scripts/rk3588/collect-evidence.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok:   $*"; }

mkdir -p "$W/bin" "$W/out"
ln -s "$ROOT/tests/rk3588/v4l2-ctl-stub" "$W/bin/v4l2-ctl"
HOST=$(hostname 2>/dev/null || echo testhost)
cat >"$W/bin/journalctl" <<STUB
#!/usr/bin/env bash
echo "2026-11-14T16:31:05-0500 $HOST hyperhdr[812]: (DDP) sending to 192.168.1.50:4048 via 02:42:ac:11:00:02"
echo "2026-11-14T16:31:06-0500 $HOST hyperhdr[812]: (V4L2) no signal: no source detected (power_present=0) on /dev/video1"
echo "2026-11-14T16:31:07-0500 $HOST kernel: snps_hdmirx fdee0000.hdmi_receiver: video2 hdmi link down, peer fe80::1c2b:3cff:fe4d:5e6f"
STUB
chmod +x "$W/bin/journalctl"

PATH="$W/bin:$PATH" RK3588_EVIDENCE_NODES="/dev/video0 /dev/video1" bash "$SCRIPT" --out "$W/out" >"$W/run.txt" 2>&1
RC=$?
[ "$RC" -eq 0 ] && pass "collector exits 0" || fail "collector exit $RC: $(tail -5 "$W/run.txt")"
ARCHIVE=$(ls "$W/out"/rk3588-evidence-*.tar.gz 2>/dev/null | head -1)
[ -n "$ARCHIVE" ] && pass "archive written" || { fail "no archive"; exit 1; }
mkdir "$W/x" && tar -C "$W/x" -xzf "$ARCHIVE"
B=$(ls -d "$W/x"/rk3588-evidence-*)
S="$B/SUMMARY.txt"

expect() { grep -qE -- "$2" "$S" && pass "$1" || fail "$1: $(grep -m1 -E "^${3:-$1}" "$S")"; }
expect "receiver found by driver, not number" '^receiver: +/dev/video1$' receiver
expect "driver and bus" '^driver/bus: +snps_hdmirx / platform:fdee0000.hdmi_receiver$' driver
expect "power_present reported" '^power_present: 0x00000000$' power
expect "RGB range control reported" '^rgb range: .*rx_rgb_quantization_range.*value=0' rgb
expect "no-signal DV timings reported" '^dv timings: .*Link has been severed' dv
expect "format and planes" "^format: +1920/1080 'BGR3'.* planes=1$" format
expect "quantization" '^quantization: +Default' quant
expect "kernel-level disclaimer" 'not HyperHDR preview or WLED evidence'
[ -f "$B/v4l2/nodes/video0.txt" ] && [ -f "$B/v4l2/nodes/video1.txt" ] && pass "every node recorded" || fail "node files missing"
[ ! -e "$B/v4l2/receiver/stream.txt" ] && [ ! -e "$B/v4l2/receiver/expbuf.txt" ] && pass "no device probes without opt-in" || fail "stream/expbuf ran without opt-in"

if grep -rqE '192\.168\.1\.50|02:42:ac:11:00:02|fe80::1c2b' "$B"; then fail "addresses leaked: $(grep -rhoE '192\.168\.1\.50|02:42:ac:11:00:02|fe80::1c2b' "$B" | sort -u | tr '\n' ' ')"; else pass "addresses redacted"; fi
grep -rqw -- "$HOST" "$B" && fail "hostname leaked" || pass "hostname redacted"
grep -q '2026-11-14T16:31:05-0500 <host>' "$B/logs/hyperhdr-journal.txt" && pass "timestamps survive redaction" || fail "timestamps damaged: $(sed -n 2p "$B/logs/hyperhdr-journal.txt")"
grep -q 'snps_hdmirx fdee0000.hdmi_receiver: video2 hdmi link down' "$B/logs/hyperhdr-journal.txt" && pass "bus info survives redaction" || fail "bus info damaged"

# Static read-only guarantee: no service, package or device-setting changes.
if grep -nE 'systemctl +(stop|start|restart|disable|enable|kill)|apt(-get)? +(install|remove|purge)|dpkg +(-i|--install|-r|--remove|-P)|--set-(ctrl|fmt|dv|parm|input)|-c +[a-z_]+=|--set-edid|rm -rf +/' "$SCRIPT" | grep -v '^\s*#'; then
	fail "collector contains a state-changing command"
else
	pass "no state-changing commands in collector"
fi
grep -nE 'stream-mmap' "$SCRIPT" | grep -vq 'STREAM' && fail "streaming outside --stream" || pass "streaming only behind --stream"

[ "$FAILS" -eq 0 ] && { echo "all evidence-collector checks passed"; exit 0; }
echo "$FAILS check(s) failed"; exit 1
