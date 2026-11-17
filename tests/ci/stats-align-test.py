#!/usr/bin/env python3
"""Hardware-free test of scripts/rk3588/stats-align.py.

The stats messages come from HyperHDR's real formatters: tests/ci/stats-lines.cpp
is compiled against include/ (V4L2Stats.h and LedOutputStats.h are Qt-free) and
its output is wrapped in journal lines the way journald and HyperHDR's logger
write them (ISO time, colour codes, padded logger name, debug location). A
format change that the parser doesn't follow fails here.
  stats-align-test.py   (needs a C++20 compiler: $CXX, default c++)
"""
import datetime as dt
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/rk3588/stats-align.py"
failures = 0


def check(cond, msg, detail=""):
    global failures
    print("%s %s" % ("ok:  " if cond else "FAIL:", msg))
    if not cond:
        failures += 1
        if detail:
            print("     " + detail.replace("\n", "\n     "))


tmp = pathlib.Path(tempfile.mkdtemp())
exe = tmp / "stats-lines"
cc = subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-I", str(ROOT / "include"), "-o", str(exe), str(ROOT / "tests/ci/stats-lines.cpp")],
                    capture_output=True, text=True)
if cc.returncode != 0:
    print("FAIL: tests/ci/stats-lines.cpp doesn't compile:\n" + cc.stderr)
    sys.exit(1)
MSG = dict(l.split("|", 1) for l in subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines())

T0 = dt.datetime(2026, 11, 16, 20, 0, 0, tzinfo=dt.timezone(dt.timedelta(hours=-5)))


def jline(t, logger, msg, level="DEBUG", precise=True):
    stamp = t.strftime("%Y-%m-%dT%H:%M:%S") + (".%06d" % t.microsecond if precise else "") + t.strftime("%z")
    loc = "/build/sources/x/File.cpp:12:report() | " if level == "DEBUG" else ""
    return "%s <host> hyperhdr[812]: \x1b[0m%s %-22s : \x1b[32;1m<%s> %s%s\x1b[36;1m" % (stamp, t.strftime("%H:%M:%S.000"), logger, level, loc, msg)


def run(lines, *args):
    p = tmp / "journal.txt"
    p.write_text("\n".join(lines) + "\n")
    return subprocess.run([sys.executable, str(SCRIPT), *args, str(p)], capture_output=True, text=True)


def at(s):
    return T0 + dt.timedelta(seconds=s)


V4L2, DDP, WLED, SPI = "V4L2:/dev/video1", "LEDDEVICE_DDP", "LEDDEVICE_WLED", "LEDDEVICE_WS281X"

# Every message from the real formatters parses, and nothing is "unrecognised".
every = [jline(at(60 * (i + 1)), V4L2 if k.startswith("capture") else DDP, m) for i, (k, m) in enumerate(MSG.items())]
r = run(every, "--json")
js = json.loads(r.stdout)
check(len(js["capture"]) == 4 and len(js["led"]["DDP"]) == 5 and not any("unrecognised" in n for n in js["notes"]),
      "every real formatter output parses", json.dumps(js["notes"]))
check(js["not_measured"] == ["capture-to-LED latency", "WLED receipt and display", "optical latency"], "JSON names what isn't measured")

# A capture interval and a DDP interval covering the same minute pair up.
good = [jline(at(60), V4L2, MSG["capture"]), jline(at(59.5), DDP, MSG["led"], "INFO")]
r = run(good)
check(r.returncode == 0 and "overlap 99%" in r.stdout and "LED device DDP" in r.stdout, "same minute: paired", r.stdout)
check("60.0 s (+-0.1 s), 30.0 fps dequeued (1800 frames), 1740 accepted, 28.98/s decoded" in r.stdout,
      "capture length recovered from counts, with its rounding uncertainty", r.stdout)
check("missed by driver sequence 2; accept->decoded p50/p95/p99 4.10/5.20/6.30 ms" in r.stdout, "capture details carried over")
check("updates 60.00/s (new colours 3600, repeats 0), failed 0; packets 10800 (3.00/update, max 3), rejected by socket 0" in r.stdout,
      "LED details carried over", r.stdout)
check("ddp: DDP on UDP 4048" in r.stdout and "Not a pipeline throughput" in r.stdout and "capture-to-LED latency: not measured" in r.stdout
      and "WLED receipt and display: not measured" in r.stdout, "transport named; rates not combined; latency and receipt unmeasured")

# Second-resolution journal (-o short-iso) still pairs.
r = run([jline(at(60), V4L2, MSG["capture"], precise=False), jline(at(60), DDP, MSG["led"], precise=False)])
check(r.returncode == 0 and "overlap 100%" in r.stdout, "short-iso timestamps", r.stdout)

# Offset by half a minute: no pair, exit 1, and it says so.
r = run([jline(at(60), V4L2, MSG["capture"]), jline(at(90), DDP, MSG["led"])])
check(r.returncode == 1 and "No capture and LED intervals cover the same time" in r.stdout, "30 s offset: not paired", r.stdout)
r = run([jline(at(60), V4L2, MSG["capture"]), jline(at(90), DDP, MSG["led"])], "--min-overlap", "0.4")
check(r.returncode == 0 and "overlap 50%" in r.stdout, "--min-overlap 0.4 accepts a 50% overlap", r.stdout)

# A partial interval at a stop: 372 frames at 30.0 fps is 12.4 s.
r = run([jline(at(12.4), V4L2, MSG["capture-stopped"], "INFO"), jline(at(12.4), DDP, MSG["led-disabled"])])
check(r.returncode == 0 and "(capture stopped): 12.4 s" in r.stdout and "(device disabled): 12.4 s" in r.stdout, "partial intervals pair", r.stdout)

# A 12.4 s capture interval inside a minute-long LED interval covers only 21%
# of it: the overlap must cover most of *each* interval.
r = run([jline(at(30), V4L2, MSG["capture-stopped"], "INFO"), jline(at(60), DDP, MSG["led"])])
check(r.returncode == 1 and "No capture and LED intervals" in r.stdout, "short interval nested in a long one: not paired", r.stdout)

# ACCOUNTING MISMATCH: length unknown, never paired.
r = run([jline(at(60), V4L2, MSG["capture-mismatch"], "INFO"), jline(at(60), DDP, MSG["led"])])
check(r.returncode == 1 and "has ACCOUNTING MISMATCH: its length can't be recovered; not paired" in r.stdout, "mismatch: not paired", r.stdout)

# Unavailable stays n/a.
r = run([jline(at(60), V4L2, MSG["capture-na"]), jline(at(60), SPI, MSG["led-nopackets"])])
check(r.returncode == 0 and "missed by driver sequence n/a; accept->decoded p50/p95/p99 n/a/n/a/n/a ms" in r.stdout
      and "packets n/a (driver doesn't use the UDP provider)" in r.stdout, "unavailable values printed as n/a", r.stdout)
r = run([jline(at(60), V4L2, MSG["capture"]), jline(at(60), DDP, MSG["led-idle"])])
check(r.returncode == 0 and "no LED output in this interval" in r.stdout, "idle LED interval reported, not as a rate", r.stdout)

# wled is not DDP.
r = run([jline(at(60), V4L2, MSG["capture"]), jline(at(60), WLED, MSG["led"])])
check("wled: WLED realtime UDP (DRGB/DNRGB)" in r.stdout and "21324 by default; not DDP" in r.stdout and "LED device WLED" in r.stdout,
      "wled driver named as WLED realtime UDP, not DDP", r.stdout)
r = run([jline(at(60), V4L2, MSG["capture"]), jline(at(60), WLED, MSG["led"]), jline(at(60), DDP, MSG["led-repeats"])])
check(r.returncode == 0 and r.stdout.count("== ") == 2 and "failed 3" in r.stdout and "repeats 1800" in r.stdout, "two LED devices, each paired", r.stdout)

# Missing lines are explained, never zero.
r = run([jline(at(60), DDP, MSG["led"])])
check(r.returncode == 1 and "no capture stats lines" in r.stdout and "set the V4L2 grabber's log level to debug" in r.stdout
      and "Not evidence of zero frames" in r.stdout, "no capture lines: debug-level hint, not zero", r.stdout)
r = run([jline(at(60), V4L2, MSG["capture"])])
check(r.returncode == 1 and "no LED output stats lines" in r.stdout and "Not evidence of zero updates" in r.stdout, "no LED lines: hint, not zero", r.stdout)

# Lines without a journal timestamp (HyperHDR console output) are ignored with a note.
r = run([jline(at(60), V4L2, MSG["capture"]).split(" ", 3)[3], jline(at(60), DDP, MSG["led"])])
check("1 stats line(s) without an ISO journal timestamp ignored" in r.stdout, "console lines without a date: noted", r.stdout)

# Other log lines, including one quoting "LED output stats", are ignored.
r = run(good + [jline(at(61), V4L2, 'Started'), "2026-11-16T20:01:02-0500 <host> kernel: unrelated"])
check(r.returncode == 0 and "capture stats lines: 1; LED output stats lines: DDP 1" in r.stdout, "unrelated lines ignored", r.stdout)

r = subprocess.run([sys.executable, str(SCRIPT), str(tmp / "missing.txt")], capture_output=True, text=True)
check(r.returncode == 2, "unreadable input: exit 2")

print("%d failure(s)" % failures if failures else "all stats-align checks passed")
sys.exit(1 if failures else 0)
