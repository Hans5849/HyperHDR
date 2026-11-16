#!/usr/bin/env python3
"""Tests for scripts/rk3588/bench-summarize.py.

  bench-summarize-test.py SAMPLE_DIR

SAMPLE_DIR holds the synthetic runs written by `bench-report-test --emit-sample`
(tests/bench_report), so this also checks that the C++ writer's JSON is what the
summarizer accepts. The synthetic A55 run is exactly 2.5x the A76 run; in every
run the full-frame case is exactly 4x the quarter-frame case.
"""
import copy
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/rk3588/bench-summarize.py"
failures = 0


def check(cond, msg):
    global failures
    print(("PASS " if cond else "FAIL ") + msg)
    failures += 0 if cond else 1


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


sample = pathlib.Path(sys.argv[1])
a76, a55 = sample / "bgr24-bench-a76.json", sample / "bgr24-bench-a55.json"
check(a76.exists() and a55.exists(), "sample runs from the C++ writer exist")

# --- the C++ JSON is valid and complete ------------------------------------------
doc = json.loads(a76.read_text())
check(doc["schema"] == "hyperhdr.bgr24-bench/1" and len(doc["cases"]) == 16, "C++ writer: schema and 16 cases (2 patterns x 8)")
check(doc["metadata"]["before"]["cur_freq_khz"] == 0 and doc["metadata"]["after"]["cur_freq_khz"] is None,
      "C++ writer: an observed 0 kHz stays 0, an unreadable frequency is null")
check(all("raw_ns" in c and len(c["raw_ns"]) == c["samples"] for c in doc["cases"]), "C++ writer: raw samples present with --raw")

# --- determinism -----------------------------------------------------------------
for fmt in ("markdown", "csv", "json"):
    r1, r2 = run("--format", fmt, a76, a55), run("--format", fmt, a55, a76)
    check(r1.returncode == 0 and r1.stdout == r2.stdout and r1.stdout, "%s output is identical whatever the file order" % fmt)

s = json.loads(run("--format", "json", a76, a55).stdout)
check(s["baseline_core"] == "Cortex-A76", "default baseline is the faster core")
check(all(r["raw_verified"] == 16 for r in s["runs"]), "every case's summary was recomputed from raw samples")
check(len(s["core_comparison"]) == 16 and all(abs(c["ratio"] - 2.5) < 1e-12 for c in s["core_comparison"]),
      "Cortex-A55 vs Cortex-A76: 2.50x in all 16 groups")
check(len(s["full_vs_quarter"]) == 16 and all(abs(c["full_over_quarter"] - 4.0) < 1e-12 for c in s["full_vs_quarter"]),
      "full vs quarter: 4.00x for every core, pattern, range and LUT state")
lut = {(c["core"], c["pattern"], c["range"], c["output"]): c["lut_over_nolut"] for c in s["lut_cost"]}
check(abs(lut[("Cortex-A76", "random", "full", "full")] - 50 / 0.6) < 1e-9, "LUT cost: 50 ms / 0.6 ms = 83.33x (random, full)")
check(abs(lut[("Cortex-A76", "gradient", "full", "full")] - 3 / 0.6) < 1e-9, "LUT cost: 3 ms / 0.6 ms = 5.00x (gradient, full)")
md = run(a76, a55).stdout
check("| Cortex-A55 | random | full | full | yes | 1 | 131.250 |" in md, "markdown row: 52.5 ms x 2.5 = 131.250 ms")
check("2.50x" in md and "4.00x" in md and "83.33x" in md, "markdown carries the comparison ratios")
s55 = json.loads(run("--format", "json", "--baseline", "Cortex-A55", a76, a55).stdout)
check(all(abs(c["ratio"] - 0.4) < 1e-12 for c in s55["core_comparison"]), "--baseline Cortex-A55 inverts the ratio (0.40x)")
r = run("--baseline", "Cortex-A78", a76)
check(r.returncode == 1 and "baseline core" in r.stderr, "an unknown baseline core is an error")

with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)

    def variant(name, change):
        v = copy.deepcopy(doc)
        change(v)
        p = d / name
        p.write_text(json.dumps(v))
        return p

    # --- invalid input fails loudly -----------------------------------------------
    def bump_median(v):
        v["cases"][3]["median_ns"] += 1
    r = run(variant("tampered.json", bump_median))
    check(r.returncode == 1 and "raw samples disagree" in r.stderr and "median_ns" in r.stderr, "a summary that disagrees with its raw samples is rejected")

    def drop_sample(v):
        v["cases"][0]["raw_ns"].pop()
    r = run(variant("short.json", drop_sample))
    check(r.returncode == 1 and "raw samples, summary says" in r.stderr, "a raw sample count that disagrees with 'samples' is rejected")

    r = run(variant("schema.json", lambda v: v.update(schema="hyperhdr.bgr24-bench/0")))
    check(r.returncode == 1 and "schema" in r.stderr, "an unknown schema is rejected")

    def drop_field(v):
        del v["cases"][2]["p95_ns"]
    r = run(variant("field.json", drop_field))
    check(r.returncode == 1 and "lacks p95_ns" in r.stderr, "a case missing a statistic is rejected")
    (d / "broken.json").write_text("{")
    check(run(d / "broken.json").returncode == 1, "malformed JSON is rejected")

    # Without raw samples the file is still accepted, just not recomputed.
    def no_raw(v):
        for c in v["cases"]:
            c.pop("raw_ns")
    s = json.loads(run("--format", "json", variant("noraw.json", no_raw)).stdout)
    check(s["runs"][0]["raw_verified"] == 0 and len(s["rows"]) == 16, "runs without raw samples are summarized, marked unverified")

    # --- missing metadata is n/a, never 0 ---------------------------------------------
    def bare(v):
        v["metadata"] = {"before": {}, "after": {}}
        for c in v["cases"]:
            c["lut_access"] = None
    p = variant("bare.json", bare)
    md = run(p).stdout
    check("| unknown | n/a | n/a | 16 (16) |" in md, "missing core, commit and CPU print as unknown / n/a")
    for note in ("not pinned to one CPU", "governor unknown", "load unknown", "frequency unknown", "temperature unknown", "commit unknown"):
        check(note in md, "missing metadata is reported: " + note)
    check("| n/a |\n" in md and "| 0 |\n" not in md, "missing LUT access profile is n/a, not 0")
    csv_out = run("--format", "csv", p).stdout
    check(",NA,NA\n" in csv_out, "CSV uses NA for missing spread and LUT lines")
    check(json.loads(run("--format", "json", p).stdout)["rows"][0]["lut_lines_64"] is None, "JSON uses null for missing values")

    # --- conditions that make a run not a baseline -------------------------------------
    def noisy(v):
        m = v["metadata"]
        m["governor"] = "schedutil"
        m["before"]["loadavg"] = [2.5, 1.0, 0.5]
        m["before"]["cur_freq_khz"], m["after"]["cur_freq_khz"] = 2400000, 1800000
        m["after"]["thermal"] = [{"zone": "thermal_zone1", "type": "bigcore0-thermal", "milli_c": 85000}]
    md = run(variant("noisy.json", noisy)).stdout
    for note in ("governor schedutil", "load 2.50 > 1.0", "frequency changed 2400000 -> 1800000 kHz", "hot: 85.0 C"):
        check(note in md, "condition reported: " + note)

    # --- repeated runs -----------------------------------------------------------------
    def slower(v):
        for c in v["cases"]:
            c["raw_ns"] = [x * 11 // 10 for x in c["raw_ns"]]
            for k, val in zip(("min_ns", "median_ns", "p95_ns", "p99_ns", "max_ns"), (lambda s: (s[0], s[len(s) * 50 // 100], s[len(s) * 95 // 100], s[len(s) * 99 // 100], s[-1]))(sorted(c["raw_ns"]))):
                c[k] = val
    s = json.loads(run("--format", "json", a76, variant("again.json", slower)).stdout)
    row = next(r for r in s["rows"] if (r["pattern"], r["range"], r["output"], r["lut"]) == ("random", "full", "full", True))
    # medians 52.5 ms and 57.75 ms: median of medians 55.125 ms, spread 5.25/55.125 = 9.52 %
    check(row["runs"] == 2 and row["median_ns"] == 55125000 and abs(row["spread_pct"] - 100 * 5.25 / 55.125) < 1e-9,
          "two runs: median of medians and the spread between them")

print("%d failure(s)" % failures if failures else "all bench-summarize checks passed")
sys.exit(1 if failures else 0)
