#!/usr/bin/env python3
"""Optical latency: test pattern schedule and analysis of a two-channel recording.

  optical-latency.py schedule [--seed N] [--count N] [--min-ms 300] [--max-ms 1500] [--fps 60] OUTDIR
  optical-latency.py analyze  [--max-latency-ms 250] [--min-latency-ms -100] [--envelope-ms 3] [--json] RECORDING.csv
  optical-latency.py analyze  --onsets [--resolution-ms R] [--json] ONSETS.csv

Method and its limits: docs/rk3588-latency-measurement.md. Standard library
only; runs nothing on the board and needs no hardware.

schedule  writes black.png, white.png, an ffmpeg concat list (pattern.txt) with
          black/white segments of random whole-frame lengths, and schedule.csv
          (the transition frames, for checking that every transition was
          recorded). Render: see the doc.
analyze   RECORDING.csv: time in seconds, then two channels: the reference
          (the TV patch, or another reference light) and the LED. A header row
          is allowed. Each channel is turned into an envelope (moving maximum
          over --envelope-ms, which hides LED and backlight PWM), its low and
          high levels are its 5th and 95th percentiles, and a transition is a
          crossing of 50% between them, with hysteresis (40%/60%), timed by
          linear interpolation. Both channels get the same filter, so its delay
          cancels in the difference. Each reference transition is paired with
          the first LED transition in the same direction in
          [--min-latency-ms, --max-latency-ms] after it; each LED transition is
          used once.
          --onsets: ONSETS.csv has channel (ref|led), time_s, direction
          (rise|fall) rows, for example read off a high-speed video frame by
          frame. --resolution-ms is the frame interval: each onset is then
          uncertain by up to one frame, which is reported.
Reported per direction and together: count, p50/p95/p99 (nearest rank), mean,
standard deviation, min/max, and a bootstrap 95% interval of the median (seeded,
reproducible), plus every reference transition without an LED response and
every LED transition without a reference. Nothing is dropped silently.

Exit status: 0 at least one paired transition, 1 none, 2 usage or bad input.
"""
import argparse
import bisect
import collections
import csv
import json
import math
import pathlib
import random
import struct
import sys
import zlib


class InputError(Exception):
    pass


# ---------------------------------------------------------------- schedule
def png(width, height, value):
    raw = b"".join(b"\x00" + bytes([value]) * (width * 3) for _ in range(height))

    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def schedule(outdir, seed, count, min_ms, max_ms, fps, width, height):
    """Alternating black/white segments of random whole-frame lengths.

    Random lengths keep transitions from locking to the LED refresh or the
    smoothing period. Starts and ends black. Returns the transition rows."""
    rng = random.Random(seed)
    lo, hi = max(1, math.ceil(min_ms * fps / 1000)), max(1, math.floor(max_ms * fps / 1000))
    if hi < lo:
        raise InputError("--max-ms is shorter than --min-ms at %g fps" % fps)
    out = pathlib.Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "black.png").write_bytes(png(width, height, 0))
    (out / "white.png").write_bytes(png(width, height, 255))
    frames = [rng.randint(lo, hi) for _ in range(count + 1)]
    lines, rows, frame = ["ffconcat version 1.0"], [], 0
    for i, n in enumerate(frames):
        colour = "white" if i % 2 else "black"
        lines += ["file %s.png" % colour, "duration %.6f" % (n / fps)]
        if i:
            rows.append((i, frame, "%.6f" % (frame / fps), "rise" if colour == "white" else "fall"))
        frame += n
    lines.append("file %s.png" % ("white" if len(frames) % 2 == 0 else "black"))  # concat repeats the last file
    (out / "pattern.txt").write_text("\n".join(lines) + "\n")
    with open(out / "schedule.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["transition", "frame", "time_s", "direction"])
        w.writerows(rows)
    return rows, frame


# ---------------------------------------------------------------- analysis
def read_recording(path):
    t, a, b = [], [], []
    with open(path, newline="") as f:
        for n, row in enumerate(csv.reader(f), 1):
            if not row or row[0].lstrip().startswith("#"):
                continue
            try:
                vals = [float(x) for x in row[:3]]
            except ValueError:
                if n == 1:
                    continue  # header
                raise InputError("%s line %d: not three numbers: %r" % (path, n, row[:3]))
            if len(vals) < 3:
                raise InputError("%s line %d: expected time, reference, LED" % (path, n))
            if t and vals[0] <= t[-1]:
                raise InputError("%s line %d: time doesn't increase" % (path, n))
            t.append(vals[0]); a.append(vals[1]); b.append(vals[2])
    if len(t) < 10:
        raise InputError("%s: fewer than 10 samples" % path)
    return t, a, b


def envelope(t, x, window):
    """Moving maximum over the trailing window (seconds), O(n)."""
    out, q = [], collections.deque()
    for i, v in enumerate(x):
        while q and x[q[-1]] <= v:
            q.pop()
        q.append(i)
        while t[q[0]] < t[i] - window:
            q.popleft()
        out.append(x[q[0]])
    return out


def percentile(sorted_vals, p):
    if not sorted_vals:
        return None
    k = max(1, math.ceil(p / 100 * len(sorted_vals)))
    return sorted_vals[k - 1]


def transitions(t, env):
    s = sorted(env)
    low, high = percentile(s, 5), percentile(s, 95)
    span = high - low
    if span <= 0:
        return [], low, high
    mid, on, off = low + span / 2, low + span * 0.6, low + span * 0.4
    state = env[0] > mid
    out, last_cross = [], None
    for i in range(1, len(env)):
        prev, cur = env[i - 1], env[i]
        if (prev < mid) != (cur < mid):  # 50% crossing between samples i-1 and i
            last_cross = t[i - 1] + (mid - prev) / (cur - prev) * (t[i] - t[i - 1])
        if not state and cur >= on:
            state = True
            out.append((last_cross if last_cross is not None else t[i], "rise"))
        elif state and cur <= off:
            state = False
            out.append((last_cross if last_cross is not None else t[i], "fall"))
    return out, low, high


def pair(ref, led, lo, hi):
    """First unused LED transition of the same direction within [t+lo, t+hi]."""
    led_times = [x[0] for x in led]
    used, pairs, missing = set(), [], []
    for i, (tr, d) in enumerate(ref):
        nxt = ref[i + 1][0] if i + 1 < len(ref) else math.inf
        j = bisect.bisect_left(led_times, tr + lo)
        found = None
        while j < len(led) and led[j][0] <= tr + hi:
            if j not in used and led[j][1] == d:
                found = j
                break
            j += 1
        if found is None:
            missing.append({"time_s": round(tr, 6), "direction": d})
            continue
        used.add(found)
        pairs.append({"time_s": round(tr, 6), "direction": d, "latency_ms": (led[found][0] - tr) * 1000,
                      "ambiguous": led[found][0] >= nxt})
    spurious = [{"time_s": round(led[j][0], 6), "direction": led[j][1]} for j in range(len(led)) if j not in used]
    return pairs, missing, spurious


def summary(vals, seed=1):
    if not vals:
        return {"n": 0}
    s = sorted(vals)
    n = len(s)
    mean = sum(s) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in s) / (n - 1)) if n > 1 else 0.0
    rng = random.Random(seed)
    meds = sorted(percentile(sorted(rng.choice(s) for _ in range(n)), 50) for _ in range(2000))
    return {"n": n, "p50": percentile(s, 50), "p95": percentile(s, 95), "p99": percentile(s, 99), "mean": mean, "sd": sd,
            "min": s[0], "max": s[-1], "median_ci95": [percentile(meds, 2.5), percentile(meds, 97.5)],
            "p99_note": None if n >= 500 else "n < 500: p99 is close to the maximum, not a stable estimate"}


def read_onsets(path):
    ref, led = [], []
    with open(path, newline="") as f:
        for n, row in enumerate(csv.reader(f), 1):
            if not row or row[0].lstrip().startswith("#") or (n == 1 and row[0].strip().lower() == "channel"):
                continue
            if len(row) < 3 or row[0].strip() not in ("ref", "led") or row[2].strip() not in ("rise", "fall"):
                raise InputError("%s line %d: expected channel (ref|led), time_s, direction (rise|fall)" % (path, n))
            try:
                tm = float(row[1])
            except ValueError:
                raise InputError("%s line %d: time %r is not a number" % (path, n, row[1]))
            (ref if row[0].strip() == "ref" else led).append((tm, row[2].strip()))
    return sorted(ref), sorted(led)


def analyze(path, onsets=False, lo_ms=-100.0, hi_ms=250.0, env_ms=3.0, resolution_ms=None):
    meta = {}
    if onsets:
        ref, led = read_onsets(path)
        meta["input"] = "onsets"
        if resolution_ms:
            # Each onset lies somewhere in one frame interval; the difference of two
            # such independent errors is triangular on (-R, R): SD R/sqrt(6).
            meta["onset_resolution_ms"] = resolution_ms
            meta["difference_uncertainty_ms"] = {"max": resolution_ms, "sd": resolution_ms / math.sqrt(6)}
    else:
        t, a, b = read_recording(path)
        dt = (t[-1] - t[0]) / (len(t) - 1)
        ea, eb = envelope(t, a, env_ms / 1000), envelope(t, b, env_ms / 1000)
        ref, ra, rb = transitions(t, ea)
        led, la, lb = transitions(t, eb)
        meta.update(input="recording", samples=len(t), sample_interval_ms=dt * 1000, envelope_ms=env_ms,
                    duration_s=t[-1] - t[0], ref_levels=[ra, rb], led_levels=[la, lb])
        if ra == rb or la == lb:
            raise InputError("a channel never changes level: check the sensors and the column order (time, reference, LED)")
    pairs, missing, spurious = pair(ref, led, lo_ms / 1000, hi_ms / 1000)
    result = {"meta": meta, "window_ms": [lo_ms, hi_ms], "ref_transitions": len(ref), "led_transitions": len(led),
              "paired": len(pairs), "no_led_response": missing, "led_without_reference": spurious,
              "ambiguous": sum(p["ambiguous"] for p in pairs),
              "all": summary([p["latency_ms"] for p in pairs]),
              "rise": summary([p["latency_ms"] for p in pairs if p["direction"] == "rise"]),
              "fall": summary([p["latency_ms"] for p in pairs if p["direction"] == "fall"]),
              "pairs": pairs}
    return result


def fmt(v):
    return "n/a" if v is None else "%.2f" % v


def report(r):
    m = r["meta"]
    out = []
    if m["input"] == "recording":
        out.append("recording: %d samples over %.1f s, sample interval %.3f ms, envelope %.1f ms (both channels)"
                   % (m["samples"], m["duration_s"], m["sample_interval_ms"], m["envelope_ms"]))
    else:
        out.append("onsets: %s" % ("frame interval %.2f ms: each latency uncertain by up to +-%.2f ms (SD %.2f ms)"
                                    % (m["onset_resolution_ms"], m["difference_uncertainty_ms"]["max"], m["difference_uncertainty_ms"]["sd"])
                                    if "onset_resolution_ms" in m else "no --resolution-ms given: onset quantization not stated"))
    out.append("reference transitions %d, LED transitions %d, paired %d (window %g..%g ms)"
               % (r["ref_transitions"], r["led_transitions"], r["paired"], r["window_ms"][0], r["window_ms"][1]))
    for name in ("rise", "fall", "all"):
        s = r[name]
        if not s["n"]:
            out.append("  %-4s n=0" % name)
            continue
        out.append("  %-4s n=%d  p50 %s  p95 %s  p99 %s  mean %s  sd %s  min %s  max %s ms; median 95%% CI %s..%s ms%s"
                   % (name, s["n"], fmt(s["p50"]), fmt(s["p95"]), fmt(s["p99"]), fmt(s["mean"]), fmt(s["sd"]), fmt(s["min"]), fmt(s["max"]),
                      fmt(s["median_ci95"][0]), fmt(s["median_ci95"][1]), " (%s)" % s["p99_note"] if s["p99_note"] else ""))
    if r["no_led_response"]:
        out.append("NOTE %d reference transition(s) without an LED response in the window, first at %.3f s"
                   % (len(r["no_led_response"]), r["no_led_response"][0]["time_s"]))
    if r["led_without_reference"]:
        out.append("NOTE %d LED transition(s) without a reference transition, first at %.3f s"
                   % (len(r["led_without_reference"]), r["led_without_reference"][0]["time_s"]))
    if r["ambiguous"]:
        out.append("WARN %d LED response(s) came after the next reference transition: intervals are too short for this latency" % r["ambiguous"])
    out.append("Latency = LED 50%% crossing minus reference 50%% crossing. Against the TV patch this includes the TV's own "
               "display latency as a negative term; see docs/rk3588-latency-measurement.md.")
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("schedule")
    s.add_argument("outdir")
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--count", type=int, default=200, help="transitions")
    s.add_argument("--min-ms", type=float, default=300)
    s.add_argument("--max-ms", type=float, default=1500)
    s.add_argument("--fps", type=float, default=60)
    s.add_argument("--size", default="1920x1080")
    a_ = sub.add_parser("analyze")
    a_.add_argument("input")
    a_.add_argument("--onsets", action="store_true")
    a_.add_argument("--resolution-ms", type=float)
    a_.add_argument("--min-latency-ms", type=float, default=-100)
    a_.add_argument("--max-latency-ms", type=float, default=250)
    a_.add_argument("--envelope-ms", type=float, default=3)
    a_.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "schedule":
            w, h = (int(v) for v in a.size.lower().split("x"))
            rows, frames = schedule(a.outdir, a.seed, a.count, a.min_ms, a.max_ms, a.fps, w, h)
            print("%d transitions over %d frames (%.1f s at %g fps), seed %d: %s"
                  % (len(rows), frames, frames / a.fps, a.fps, a.seed, pathlib.Path(a.outdir) / "pattern.txt"))
            return 0
        if a.min_latency_ms >= a.max_latency_ms:
            raise InputError("--min-latency-ms must be below --max-latency-ms")
        r = analyze(a.input, a.onsets, a.min_latency_ms, a.max_latency_ms, a.envelope_ms, a.resolution_ms)
    except (InputError, OSError, ValueError) as e:
        print("optical-latency: %s" % e, file=sys.stderr)
        return 2
    print(json.dumps(r, indent=1) if a.json else report(r))
    return 0 if r["paired"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
