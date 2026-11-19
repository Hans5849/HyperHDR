#!/usr/bin/env python3
"""Hardware-free test of scripts/rk3588/optical-latency.py.

Synthetic two-channel recordings with known latencies: a reference light that
switches cleanly, and an LED channel delayed by a known amount per transition,
switching through 1 kHz PWM (as WS281x strips and many backlights do), with
noise, a slow rise, dropped responses and stray flashes. The analysis has to
recover the known delays, separate rising and falling edges, and report every
transition it couldn't pair.
"""
import csv
import json
import math
import pathlib
import random
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/rk3588/optical-latency.py"
failures = 0


def check(cond, msg, detail=""):
    global failures
    print("%s %s" % ("ok:  " if cond else "FAIL:", msg))
    if not cond:
        failures += 1
        if detail:
            print("     " + str(detail).replace("\n", "\n     ")[:2000])


tmp = pathlib.Path(tempfile.mkdtemp())


def run(*args):
    r = subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)
    return r


def synth(name, delays, fs=5000.0, gap=(0.3, 0.9), seed=7, pwm_hz=1000.0, noise=0.03, rise_ms=0.0, drop=(), stray=(), led_first=False):
    """delays[i]: LED latency in ms of transition i (rise, fall, rise, ...). Returns the true transition times."""
    rng = random.Random(seed)
    t, times = 0.5, []
    for _ in delays:
        times.append(t)
        t += rng.uniform(*gap)
    end = t + 0.5
    ref_on = lambda x: sum(1 for tt in times if tt <= x) % 2 == 1
    led_events = [(tt + d / 1000.0) for tt, d in zip(times, delays)]
    def led_level(x):
        k = 0
        for i, ev in enumerate(led_events):
            if i in drop:
                continue
            if ev <= x:
                k = i + 1
        state = k % 2 == 1  # after an odd-numbered transition the LED is on
        if any(s <= x < s + 0.02 for s in stray):
            state = not state
        if state and rise_ms:
            last = max(ev for i, ev in enumerate(led_events) if ev <= x and i not in drop)
            frac = min(1.0, (x - last) * 1000 / rise_ms)
        else:
            frac = 1.0 if state else 0.0
        pwm = (x * pwm_hz) % 1.0 < 0.5 if pwm_hz else True
        return frac * (1.0 if pwm else 0.0)
    path = tmp / ("%s.csv" % name)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["time_s", "tv", "led"])
        n = int(end * fs)
        for i in range(n):
            x = i / fs
            w.writerow(["%.6f" % x, "%.4f" % ((0.9 if ref_on(x) else 0.1) + rng.gauss(0, noise)),
                        "%.4f" % (0.05 + 0.9 * led_level(x) + rng.gauss(0, noise))])
    return path, times


def analyze(path, *extra):
    r = run("analyze", "--json", *extra, path)
    try:
        return r, json.loads(r.stdout)
    except ValueError:
        return r, None


# 1. Constant 42 ms, PWM LED, noise: recovered within a PWM period plus a sample.
rng = random.Random(1)
n = 60
path, _ = synth("const", [42.0] * n)
r, js = analyze(path)
tol = 1.0 + 1000 / 5000  # 1 ms PWM period + one sample
check(r.returncode == 0 and js and js["paired"] == n and abs(js["all"]["p50"] - 42.0) <= tol and abs(js["all"]["max"] - 42.0) <= tol
      and abs(js["all"]["min"] - 42.0) <= tol, "constant 42 ms through 1 kHz PWM: every transition paired, within %.1f ms" % tol,
      r.stdout[:600] + r.stderr)
check(js and js["rise"]["n"] == n // 2 and js["fall"]["n"] == n // 2 and not js["no_led_response"] and not js["led_without_reference"],
      "rising and falling edges counted separately; nothing unpaired")

# 2. Known spread: delays drawn per transition; percentiles match the truth.
delays = [30 + rng.uniform(0, 33.3) for _ in range(120)]
path, _ = synth("spread", delays, seed=11)
r, js = analyze(path)
truth = sorted(delays)
p = lambda s, q: s[max(1, math.ceil(q / 100 * len(s))) - 1]
got = [x["latency_ms"] for x in js["pairs"]] if js else []
err = max(abs(a - b) for a, b in zip(got, delays)) if len(got) == len(delays) else None
check(err is not None and err <= tol, "every latency within %.1f ms of its true value (worst %.2f)" % (tol, err or -1), r.stdout[:400])
check(js and abs(js["all"]["p50"] - p(truth, 50)) <= tol and abs(js["all"]["p95"] - p(truth, 95)) <= tol,
      "p50/p95 match the true distribution", js and js["all"])
lo, hi = js["all"]["median_ci95"] if js else (0, 0)
check(js and lo <= js["all"]["p50"] <= hi and hi - lo < 15, "bootstrap median interval brackets the median (%.2f..%.2f)" % (lo, hi))
check(js and js["all"]["p99_note"] and "n < 500" in js["all"]["p99_note"], "p99 flagged as unstable below 500 transitions")
r2, js2 = analyze(path)
check(js2 and js2["all"] == js["all"], "analysis is reproducible (seeded bootstrap)")

# 3. Different rise and fall latencies are reported apart.
path, _ = synth("asym", [25.0 if i % 2 == 0 else 60.0 for i in range(40)], seed=3)
r, js = analyze(path)
check(js and abs(js["rise"]["p50"] - 25) <= tol and abs(js["fall"]["p50"] - 60) <= tol, "rise 25 ms and fall 60 ms reported separately",
      js and (js["rise"]["p50"], js["fall"]["p50"]))

# 4. Dropped LED responses and a stray flash are reported, not hidden.
path, times = synth("drops", [40.0] * 30, seed=5, drop={6, 7}, stray=[0.4])
r, js = analyze(path)
check(js and len(js["no_led_response"]) >= 2 and len(js["led_without_reference"]) >= 1 and abs(js["all"]["p50"] - 40) <= tol,
      "dropped responses and a stray flash listed; the rest still measured",
      js and (len(js["no_led_response"]), len(js["led_without_reference"]), js["all"]["p50"]))
check("without an LED response" in run("analyze", path).stdout, "text report names unpaired transitions")

# 5. Latency beyond the window: unpaired, exit 1, never a wrong pairing.
path, _ = synth("late", [400.0] * 10, seed=9, gap=(0.9, 1.2))
r, js = analyze(path)
check(r.returncode == 1 and js and js["paired"] == 0 and len(js["no_led_response"]) == 10, "400 ms with a 250 ms window: unpaired, exit 1")
r, js = analyze(path, "--max-latency-ms", "600")
check(r.returncode == 0 and js and abs(js["all"]["p50"] - 400) <= tol, "--max-latency-ms 600 measures it")

# 6. LED ahead of the TV patch (TV slower than the LEDs): negative latency.
path, _ = synth("lead", [-35.0] * 20, seed=13)
r, js = analyze(path)
check(js and js["paired"] == 20 and abs(js["all"]["p50"] + 35) <= tol, "LED 35 ms before the TV patch: -35 ms", js and js["all"]["p50"])

# 7. Smoothing-like 40 ms rise: the 50% crossing is ~20 ms after the start.
path, _ = synth("ramp", [30.0] * 20, seed=17, rise_ms=40.0, pwm_hz=0)
r, js = analyze(path)
check(js and abs(js["rise"]["p50"] - 50.0) <= 2 and abs(js["fall"]["p50"] - 30.0) <= tol,
      "a 40 ms ramp is timed at its 50% point (rise ~50 ms, fall 30 ms)", js and (js["rise"]["p50"], js["fall"]["p50"]))

# 8. Intervals shorter than the latency: flagged as ambiguous.
path, _ = synth("ambig", [200.0] * 20, seed=19, gap=(0.12, 0.18))
r, js = analyze(path)
check(js and js["ambiguous"] > 0 and "too short for this latency" in run("analyze", path).stdout, "responses after the next transition flagged")

# 9. Onset lists (high-speed camera), with frame quantization stated.
on = tmp / "onsets.csv"
on.write_text("channel,time_s,direction\n" + "".join("ref,%.5f,%s\nled,%.5f,%s\n" % (i, d, i + 0.05, d)
                                                     for i, d in zip(range(1, 21), ["rise", "fall"] * 10)))
r, js = analyze(on, "--onsets", "--resolution-ms", "4.17")
check(r.returncode == 0 and js and js["paired"] == 20 and abs(js["all"]["p50"] - 50) < 1e-6
      and abs(js["meta"]["difference_uncertainty_ms"]["sd"] - 4.17 / math.sqrt(6)) < 1e-9, "onset list: 50 ms, frame uncertainty R/sqrt(6)")
check("each latency uncertain by up to +-4.17 ms" in run("analyze", "--onsets", "--resolution-ms", "4.17", on).stdout, "text names the frame uncertainty")
check("onset quantization not stated" in run("analyze", "--onsets", on).stdout, "missing --resolution-ms is said, not assumed")

# Nearest-rank percentiles: latencies 1..10 ms give p50 5, p95 10, p99 10.
on.write_text("".join("ref,%d,rise\nled,%.3f,rise\n" % (i * 2, i * 2 + i / 1000) for i in range(1, 11)))
r, js = analyze(on, "--onsets")
check(js and [round(js["all"][k], 6) for k in ("p50", "p95", "p99", "min", "max")] == [5, 10, 10, 1, 10], "nearest-rank percentiles",
      js and js["all"])

# An LED transition in the other direction inside the window is not a response.
on.write_text("ref,1.000,rise\nled,1.010,fall\nled,1.050,rise\n")
r, js = analyze(on, "--onsets")
check(js and js["paired"] == 1 and abs(js["all"]["p50"] - 50) < 1e-6 and len(js["led_without_reference"]) == 1,
      "a falling LED edge isn't taken as the response to a rise", js and js["pairs"])

# Dropouts: a USB recorder that loses 30 ms of samples across three LED
# edges. Interpolating a crossing across the hole could be off by up to 15 ms;
# those pairs are listed and left out, the rest stay exact.
path, times = synth("gaps", [42.0] * 30, seed=23)
holes = [(times[i] + 0.042 - 0.010, times[i] + 0.042 + 0.020) for i in (4, 11, 20)]
rows = list(csv.reader(open(path)))
with open(path, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(rows[0])
    w.writerows(r for r in rows[1:] if not any(h0 <= float(r[0]) <= h1 for h0, h1 in holes))
r, js = analyze(path)
check(js and js["meta"]["gaps"] == 3 and 29 <= js["meta"]["largest_gap_ms"] <= 31 and len(js["timed_across_gap"]) == 3,
      "three 30 ms recording gaps found; the three pairs timed across them are excluded", js and (js["meta"]["gaps"], js["meta"]["largest_gap_ms"], len(js["timed_across_gap"])))
check(js and js["paired"] == 27 and abs(js["all"]["min"] - 42) <= tol and abs(js["all"]["max"] - 42) <= tol and abs(js["meta"]["sample_interval_ms"] - 0.2) < 1e-6,
      "the other 27 stay within %.1f ms; the median sample interval ignores the gaps" % tol, js and (js["paired"], js["all"]["min"], js["all"]["max"]))
check("3 gap(s) in the recording longer than 5 sample intervals" in run("analyze", path).stdout, "text report warns about the gaps")
r, js = analyze(synth("nogaps", [42.0] * 10, seed=29)[0])
check(js and js["meta"]["gaps"] == 0 and not js["timed_across_gap"] and "gap" not in run("analyze", synth("nogaps2", [42.0] * 10, seed=29)[0]).stdout,
      "an even recording reports no gaps")

# 10. Bad input.
bad = tmp / "flat.csv"
bad.write_text("".join("%d,0.5,0.5\n" % i for i in range(100)))
check(run("analyze", bad).returncode == 2, "a channel that never changes: exit 2")
bad.write_text("t,a,b\n0,1,1\n0,1,1\n" + "".join("%d,1,1\n" % i for i in range(2, 20)))
check(run("analyze", bad).returncode == 2, "time not increasing: exit 2")
on.write_text("ref,1.0,up\n")
check(run("analyze", "--onsets", on).returncode == 2, "bad onset direction: exit 2")

# 11. Schedule: deterministic, whole frames, within bounds, alternating.
out1, out2 = tmp / "s1", tmp / "s2"
r = run("schedule", "--seed", 3, "--count", 50, "--size", "64x36", out1)
run("schedule", "--seed", 3, "--count", 50, "--size", "64x36", out2)
rows = list(csv.DictReader(open(out1 / "schedule.csv")))
gaps = [int(b["frame"]) - int(a["frame"]) for a, b in zip(rows, rows[1:])]
check(r.returncode == 0 and len(rows) == 50 and all(18 <= g <= 90 for g in gaps), "50 transitions, 300-1500 ms at 60 fps (18-90 frames)", r.stdout + r.stderr)
check([x["direction"] for x in rows[:4]] == ["rise", "fall", "rise", "fall"], "alternates starting with a rise")
check((out1 / "pattern.txt").read_text() == (out2 / "pattern.txt").read_text() and (out1 / "white.png").read_bytes() == (out2 / "white.png").read_bytes(),
      "same seed: identical pattern")
txt = (out1 / "pattern.txt").read_text().split("\n")
check(txt[0] == "ffconcat version 1.0" and txt[1] == "file black.png" and (out1 / "black.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n",
      "ffconcat list starting on black, with PNG frames")
check(run("schedule", "--min-ms", 100, "--max-ms", 10, tmp / "s3").returncode == 2, "impossible interval range: exit 2")

print("%d failure(s)" % failures if failures else "all optical-latency checks passed")
sys.exit(1 if failures else 0)
