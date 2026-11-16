#!/usr/bin/env python3
"""Tests for scripts/rk3588/bench-validate.py and the benchmark JSON specification.

  bench-validate-test.py SAMPLE_DIR

SAMPLE_DIR holds the synthetic runs written by `bench-report-test --emit-sample`
(tests/bench_report), so the C++ writer's output is checked against the spec.
Each defect below is applied to a valid document and must be rejected with the
expected message: a validator that accepts everything fails here.
"""
import copy
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/rk3588/bench-validate.py"
sys.path.insert(0, str(SCRIPT.parent))
import importlib.util  # noqa: E402

spec_ = importlib.util.spec_from_file_location("bench_validate", SCRIPT)
bv = importlib.util.module_from_spec(spec_)
spec_.loader.exec_module(bv)

failures = 0


def check(cond, msg):
    global failures
    print(("PASS " if cond else "FAIL ") + msg)
    failures += 0 if cond else 1


class Opts:
    iterations = warmup = patterns = input_sha256 = None
    raw = False


sample = pathlib.Path(sys.argv[1])
valid = json.loads((sample / "bgr24-bench-a76.json").read_text())
spec = bv.load_spec()
check(bv.validate(valid, Opts()) == [], "C++ writer sample is valid")

o = Opts()
o.iterations, o.warmup, o.patterns, o.raw = 11, 3, ["random", "gradient"], True
check(bv.validate(valid, o) == [], "sample matches its iterations, warm-up, patterns and raw samples")

# --- the spec covers exactly what the writer emits ----------------------------------
for kind, key in (("run", None), ("metadata", "metadata"), ("config", "config")):
    obj = valid if key is None else valid[key]
    check(list(obj) == [f["name"] for f in spec["objects"][kind] if f["name"] in obj], "%s: fields and order match the spec" % kind)
case_names = [f["name"] for f in spec["objects"]["case"]]
check(list(valid["cases"][0]) == case_names, "case: every documented field, in order (with raw_ns)")


def mutated(fn):
    d = copy.deepcopy(valid)
    fn(d)
    return d


def rejects(fn, expect, msg, opts=None):
    errors = bv.validate(mutated(fn), opts or Opts())
    ok = any(re.search(expect, e) for e in errors)
    check(ok, "rejects %s" % msg)
    if not ok:
        print("     got: %s" % (errors or "no errors"))


def setkey(path, value):
    def fn(d):
        *head, last = path
        t = d
        for k in head:
            t = t[k]
        t[last] = value
    return fn


def delkey(path):
    def fn(d):
        *head, last = path
        t = d
        for k in head:
            t = t[k]
        del t[last]
    return fn


# schema / version
rejects(setkey(["schema"], "hyperhdr.bgr24-bench/2"), r"\$\.schema: expected 'hyperhdr.bgr24-bench/1'", "a different schema version")
rejects(delkey(["schema"]), r"\$\.schema: missing required field", "a missing schema identifier")
# field names, required/optional, undocumented
rejects(delkey(["cases", 0, "p99_ns"]), r"cases\[0\]\.p99_ns: missing required field", "a missing required case field")
rejects(delkey(["metadata", "governor"]), r"metadata\.governor: missing required field", "a missing nullable field (null, not absent)")
rejects(setkey(["cases", 0, "p99"], 5), r"undocumented field\(s\): p99", "an undocumented field")
check(bv.validate(mutated(delkey(["config", "input"])), Opts()) == [], "accepts a file written before config.input existed (optional field)")
check(bv.validate(mutated(lambda d: [c.pop("raw_ns") for c in d["cases"]]), Opts()) == [], "accepts a run without --raw")
rejects(lambda d: [c.pop("raw_ns") for c in d["cases"]], r"raw_ns: missing \(run with --raw\)", "missing raw samples when --raw was expected",
        type("O", (Opts,), {"raw": True})())
# types and units
rejects(setkey(["cases", 0, "median_ns"], 1.5), r"median_ns: expected integer, got float", "a fractional nanosecond count")
rejects(setkey(["cases", 0, "lut"], 1), r"\.lut: expected boolean", "1 instead of true")
rejects(setkey(["cases", 0, "samples"], True), r"samples: expected integer, got bool", "true instead of a count")
rejects(setkey(["cases", 0, "first_ns"], -1), r"first_ns: negative ns", "a negative duration")
rejects(setkey(["metadata", "before", "loadavg"], [0.1, 0.2]), r"loadavg: expected 3 items", "a load average with two values")
rejects(setkey(["config", "time_unit"], "us"), r"time_unit: expected 'ns'", "a different time unit")
rejects(setkey(["config", "percentile_rule"], "nearest-rank"), r"percentile_rule: expected", "a different percentile rule")
rejects(setkey(["cases", 0, "range"], "auto"), r"range: 'auto' not in", "an unknown range")
# null versus zero
rejects(setkey(["cases", 0, "median_ns"], None), r"median_ns: null, but the field is not nullable", "null for a measured value")
rejects(setkey(["metadata", "pinned_cpu"], None), r"pinned_cpu: None, but affinity \[4\] means 4", "an unpinned run with a one-CPU affinity")
rejects(lambda d: d["metadata"].update(affinity=[4, 5], pinned_cpu=None), r"metadata\.governor: has a value but the run wasn't pinned",
        "per-core data on an unpinned run (would describe the wrong core)")
check(bv.validate(mutated(lambda d: d["metadata"].update(governor=None, before=dict(d["metadata"]["before"], cur_freq_khz=None))), Opts()) == [],
      "accepts null for unreadable governor and frequency")
check(valid["metadata"]["before"]["cur_freq_khz"] == 0 and bv.validate(valid, Opts()) == [], "accepts an observed 0 kHz (zero is a value, not missing)")
rejects(setkey(["cases", 0, "stddev_ns"], None), r"stddev_ns: None, recomputed", "a missing deviation with enough samples")
rejects(lambda d: d["cases"][0].update(raw_ns=[7], samples=1, min_ns=7, median_ns=7, p95_ns=7, p99_ns=7, max_ns=7, mean_ns=7.0, stddev_ns=0.0),
        r"stddev_ns: must be null below two samples", "a deviation of one sample written as 0")
# raw sample lengths and statistics
rejects(lambda d: d["cases"][0]["raw_ns"].pop(), r"raw_ns: 10 samples, the summary says 11", "raw samples shorter than the count")
rejects(setkey(["cases", 0, "median_ns"], valid["cases"][0]["median_ns"] + 1), r"median_ns: \d+, recomputed from raw_ns", "a median that raw samples don't give")
# 11 samples: p95 = sorted[floor(11*95/100)] = sorted[10] (the maximum); sorted[9] is the nearest-rank-style mistake
rejects(setkey(["cases", 0, "p95_ns"], sorted(valid["cases"][0]["raw_ns"])[9]), r"p95_ns: \d+, recomputed", "a p95 off by the percentile rule")
rejects(setkey(["cases", 0, "mean_ns"], valid["cases"][0]["mean_ns"] + 1), r"mean_ns: .*recomputed", "a wrong mean")
rejects(lambda d: d["cases"][0].update(p99_ns=d["cases"][0]["min_ns"] - 1, raw_ns=None) or d["cases"][0].pop("raw_ns"), r"not ordered", "p99 below the minimum")
# warm-up and iteration counts
rejects(lambda d: None, r"samples: 11, expected --iterations 200", "a sample count other than --iterations", type("O", (Opts,), {"iterations": 200})())
rejects(lambda d: None, r"warmup: 3, expected --warmup 10", "a warm-up other than --warmup", type("O", (Opts,), {"warmup": 10})())
# input identification and ordering
rejects(setkey(["cases", 0, "scenario"], "full-full-nolut-gradient"), r"scenario: .*expected 'full-full-nolut-random'", "a scenario that disagrees with its fields")
rejects(setkey(["cases", 0, "pattern"], "noise"), r"pattern: 'noise' doesn't match", "an unknown pattern")
rejects(lambda d: d["cases"].__setitem__(slice(0, 2), [d["cases"][1], d["cases"][0]]), r"out of order", "swapped cases")
rejects(lambda d: d["cases"].pop(), r"15 cases; expected 8 per pattern", "a missing case")
rejects(lambda d: None, r"patterns \['random', 'gradient'\], expected \['gradient', 'random'\]", "patterns in a different order than requested",
        type("O", (Opts,), {"patterns": ["gradient", "random"]})())
check(valid["cases"][1]["lut"] and not valid["cases"][0]["lut"], "sample case 0 is without LUT, case 1 with")
rejects(setkey(["cases", 1, "lut_access"], None), r"lut_access: must be present exactly for LUT cases", "a LUT case without its access profile")
rejects(setkey(["cases", 0, "lut_access"], valid["cases"][1]["lut_access"]), r"lut_access: must be present exactly", "an access profile on a case without LUT")
rejects(setkey(["cases", 0, "output_width"], 1919), r"output 1919x1080, expected 1920x1080", "a wrong output size")
rejects(setkey(["cases", 2, "output_height"], 1080), r"expected 960x540 for quarter output", "a quarter output that isn't (w>>1)x(h>>1)")
# file input
sha = "ab" * 32
as_file = lambda d: (d["config"].__setitem__("input", {"kind": "file", "name": "f.bgr24", "sha256": sha, "format": "BGR24", "declared_range": "limited",
                                                      "orientation": "top-down", "payload_bytes": 6220800, "source": None}),
                     [c.update(pattern="file:" + sha[:12], scenario=c["scenario"].rsplit("-", 1)[0] + "-file:" + sha[:12]) for c in d["cases"][:8]],
                     d["cases"].__delitem__(slice(8, None)))
check(bv.validate(mutated(as_file), Opts()) == [], "accepts a file input identified by its sha256")
check(bv.validate(mutated(as_file), type("O", (Opts,), {"input_sha256": sha})()) == [], "file input matches --input-sha256")
rejects(lambda d: (as_file(d), d["config"]["input"].update(sha256=None)), r"config\.input\.sha256: null for a file input", "a file input without its hash")
rejects(lambda d: (as_file(d), [c.update(pattern="file:000000000000", scenario=c["scenario"].rsplit("-", 1)[0] + "-file:000000000000") for c in d["cases"]]),
        r"doesn't identify the input file", "a file pattern that isn't the input's hash")
rejects(setkey(["config", "input", "name"], "x.bgr24"), r"config\.input\.name: has a value for synthetic input", "file fields on a synthetic input")
rejects(lambda d: None, r"synthetic, expected the file input", "a synthetic run when a file input was expected",
        type("O", (Opts,), {"input_sha256": sha})())

# --- command line, and reproducible output ------------------------------------------
r = subprocess.run([sys.executable, str(SCRIPT), "--raw", str(sample / "bgr24-bench-a76.json"), str(sample / "bgr24-bench-a55.json")],
                   capture_output=True, text=True)
check(r.returncode == 0 and r.stdout.count(": valid") == 2, "command line: both sample files valid")
bad = sample.parent / "bench-validate-bad.json"
bad.write_text(json.dumps(mutated(setkey(["cases", 0, "lut"], "no"))))
r = subprocess.run([sys.executable, str(SCRIPT), str(bad)], capture_output=True, text=True)
check(r.returncode == 1 and "expected boolean" in r.stdout, "command line: exit 1 and the reason for an invalid file")
bad.unlink()


# --- docs: the field table is the spec ----------------------------------------------
doc = (ROOT / "docs/rk3588-benchmark-reporting.md").read_text()
table = bv.markdown(spec)
check(table in doc, "docs/rk3588-benchmark-reporting.md contains the field table generated from the spec (bench-validate.py --markdown)")
documented = set(re.findall(r"^\| `([^`]+)` \|", doc, re.M))
generated = set(re.findall(r"^\| `([^`]+)` \|", table, re.M))
check(documented >= generated and len(generated) == sum(len(v) for v in spec["objects"].values()), "every specified field is documented once")

print("%d failure(s)" % failures if failures else "all bench-validate checks passed")
sys.exit(1 if failures else 0)
