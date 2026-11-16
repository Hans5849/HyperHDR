#!/usr/bin/env python3
"""Validate `DecoderTest --bgr24-bench --json` output against its specification.

  bench-validate.py [--iterations N] [--warmup N] [--patterns LIST] [--raw]
                    [--input-sha256 HEX] FILE...
  bench-validate.py --markdown          print the field table for the docs

The specification is scripts/rk3588/bench-schema.json: every field's name, type,
unit, nullability and required/optional status, in output order. Standard library
only, so it runs on the board runner and in hosted CI.

Structural checks (every file):
  * exact field set and order per object; no undocumented field; types; enums,
    constants and patterns; array item types and lengths;
  * null only where the field is nullable. Missing is never written as 0, so a
    per-core field must be null when the run wasn't pinned to one CPU, and the
    file fields of config.input are null for synthetic input.
Semantic checks:
  * scenario is range-output-lut|nolut-pattern; full output is width x height,
    quarter is (width >> 1) x (height >> 1); lut_access present exactly for LUT
    cases;
  * min <= median <= p95 <= p99 <= max; raw_ns has `samples` values and
    reproduces min/median/p95/p99/max exactly, mean and standard deviation to
    the written precision;
  * cases come in the benchmark's order: per pattern, range (full, limited) x
    output (full, quarter) x LUT (no, yes);
  * a file input's pattern is file:<first 12 hex of its sha256>.
With options, also: samples == --iterations, warmup == --warmup, the patterns
are exactly --patterns in that order, raw_ns present (--raw), and the input's
sha256 (--input-sha256).

Exit status: 0 valid, 1 invalid, 2 usage.
"""
import argparse
import json
import math
import pathlib
import re
import sys

SPEC_PATH = pathlib.Path(__file__).resolve().parent / "bench-schema.json"
TYPES = {"string": str, "integer": int, "number": (int, float), "boolean": bool, "object": dict, "array": list}
ORDER = [(r, o, l) for r in ("full", "limited") for o in ("full", "quarter") for l in (False, True)]


def load_spec(path=SPEC_PATH):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def is_type(value, name):
    if name in ("integer", "number") and isinstance(value, bool):
        return False
    return isinstance(value, TYPES[name])


class Validator:
    def __init__(self, spec):
        self.spec = spec
        self.errors = []

    def err(self, where, msg):
        self.errors.append("%s: %s" % (where, msg))

    def obj(self, value, kind, where, ctx):
        fields = self.spec["objects"][kind]
        if not isinstance(value, dict):
            return self.err(where, "expected an object")
        names = [f["name"] for f in fields]
        extra = [k for k in value if k not in names]
        if extra:
            self.err(where, "undocumented field(s): " + ", ".join(extra))
        present = [n for n in names if n in value]
        if [k for k in value if k in names] != present:
            self.err(where, "fields out of order: %s, expected %s" % (", ".join(k for k in value if k in names), ", ".join(present)))
        for f in fields:
            w = where + "." + f["name"]
            if f["name"] not in value:
                if f.get("required", True):
                    self.err(w, "missing required field")
                continue
            self.field(value[f["name"]], f, w, ctx)

    def field(self, v, f, where, ctx):
        if v is None:
            if not f.get("nullable"):
                self.err(where, "null, but the field is not nullable")
            return
        if f.get("pinned_only") and not ctx.get("pinned"):
            self.err(where, "has a value but the run wasn't pinned to one CPU (per-core data must be null)")
        if f.get("file_only") and ctx.get("input_kind") == "synthetic":
            self.err(where, "has a value for synthetic input (must be null)")
        t = f["type"]
        if not is_type(v, t):
            return self.err(where, "expected %s, got %s" % (t, type(v).__name__))
        if "const" in f and v != f["const"]:
            self.err(where, "expected %r, got %r" % (f["const"], v))
        if "enum" in f and v not in f["enum"]:
            self.err(where, "%r not in %s" % (v, f["enum"]))
        if "pattern" in f and not re.match(f["pattern"], v):
            self.err(where, "%r doesn't match %s" % (v, f["pattern"]))
        if t == "integer" and f.get("unit") in ("ns", "bytes", "px", "count", "calls", "kHz") and v < 0:
            self.err(where, "negative %s" % f["unit"])
        if t == "object":
            self.obj(v, f["object"], where, ctx)
        if t == "array":
            if "length" in f and len(v) != f["length"]:
                self.err(where, "expected %d items, got %d" % (f["length"], len(v)))
            if len(v) < f.get("min_items", 0):
                self.err(where, "expected at least %d item(s)" % f["min_items"])
            for i, item in enumerate(v):
                if f["items"] == "object":
                    self.obj(item, f["object"], "%s[%d]" % (where, i), ctx)
                elif not is_type(item, f["items"]):
                    self.err("%s[%d]" % (where, i), "expected %s, got %s" % (f["items"], type(item).__name__))


def rank(s, p):
    return s[min(len(s) - 1, (len(s) * p) // 100)]


def semantics(doc, v, opts):
    cfg, cases = doc.get("config") or {}, doc.get("cases") or []
    width, height = cfg.get("width"), cfg.get("height")
    inp = cfg.get("input") or {"kind": "synthetic"}
    if inp.get("kind") == "file":
        for k in ("name", "sha256", "format", "declared_range", "orientation", "payload_bytes"):
            if inp.get(k) is None:
                v.err("config.input." + k, "null for a file input")
        if inp.get("sha256") is not None and not re.fullmatch(r"[0-9a-f]{64}", inp["sha256"]):
            v.err("config.input.sha256", "not 64 lower-case hex digits")
        if opts.input_sha256 and inp.get("sha256") != opts.input_sha256:
            v.err("config.input.sha256", "%s, expected %s" % (inp.get("sha256"), opts.input_sha256))
    elif opts.input_sha256:
        v.err("config.input", "synthetic, expected the file input %s" % opts.input_sha256)

    patterns = []
    for i, c in enumerate(cases):
        w = "cases[%d]" % i
        if not isinstance(c, dict) or not all(k in c for k in ("range", "output", "lut", "pattern", "samples", "min_ns", "median_ns", "p95_ns", "p99_ns", "max_ns")):
            continue
        if not patterns or patterns[-1] != c["pattern"]:
            patterns.append(c["pattern"])
        expect = "%s-%s-%s-%s" % (c["range"], c["output"], "lut" if c["lut"] else "nolut", c["pattern"])
        if c.get("scenario") != expect:
            v.err(w + ".scenario", "%r, expected %r" % (c.get("scenario"), expect))
        if (c["range"], c["output"], c["lut"]) != ORDER[i % 8]:
            v.err(w, "out of order: %s, expected %s-%s-%s" % (expect, ORDER[i % 8][0], ORDER[i % 8][1], "lut" if ORDER[i % 8][2] else "nolut"))
        if isinstance(width, int) and isinstance(height, int):
            dims = (width, height) if c["output"] == "full" else (width >> 1, height >> 1)
            if (c.get("output_width"), c.get("output_height")) != dims:
                v.err(w, "output %sx%s, expected %dx%d for %s output" % (c.get("output_width"), c.get("output_height"), dims[0], dims[1], c["output"]))
        if c["lut"] != (c.get("lut_access") is not None):
            v.err(w + ".lut_access", "must be present exactly for LUT cases")
        if inp.get("kind") == "file" and inp.get("sha256") and c["pattern"] != "file:" + inp["sha256"][:12]:
            v.err(w + ".pattern", "%r doesn't identify the input file %s" % (c["pattern"], inp["sha256"][:12]))
        if inp.get("kind") != "file" and c["pattern"].startswith("file:"):
            v.err(w + ".pattern", "file pattern for synthetic input")
        stats = [c[k] for k in ("min_ns", "median_ns", "p95_ns", "p99_ns", "max_ns")]
        if all(isinstance(x, int) for x in stats) and stats != sorted(stats):
            v.err(w, "statistics not ordered min <= median <= p95 <= p99 <= max: %s" % stats)
        if opts.iterations is not None and c["samples"] != opts.iterations:
            v.err(w + ".samples", "%s, expected --iterations %d" % (c["samples"], opts.iterations))
        if opts.warmup is not None and c.get("warmup") != opts.warmup:
            v.err(w + ".warmup", "%s, expected --warmup %d" % (c.get("warmup"), opts.warmup))
        raw = c.get("raw_ns")
        if raw is None:
            if opts.raw:
                v.err(w + ".raw_ns", "missing (run with --raw)")
            continue
        if not isinstance(raw, list) or not raw or not all(isinstance(x, int) and not isinstance(x, bool) for x in raw):
            continue   # structural error already reported
        if len(raw) != c["samples"]:
            v.err(w + ".raw_ns", "%d samples, the summary says %s" % (len(raw), c["samples"]))
            continue
        s = sorted(raw)
        got = {"min_ns": s[0], "median_ns": rank(s, 50), "p95_ns": rank(s, 95), "p99_ns": rank(s, 99), "max_ns": s[-1]}
        for k, val in got.items():
            if c[k] != val:
                v.err(w + "." + k, "%s, recomputed from raw_ns %d" % (c[k], val))
        n = len(raw)
        mean = sum(raw) / n
        if isinstance(c.get("mean_ns"), (int, float)) and abs(c["mean_ns"] - mean) > 1e-6 + 1e-9 * abs(mean):
            v.err(w + ".mean_ns", "%s, recomputed %.6f" % (c["mean_ns"], mean))
        sd = c.get("stddev_ns")
        if n < 2:
            if sd is not None:
                v.err(w + ".stddev_ns", "must be null below two samples")
        else:
            want = math.sqrt(sum((x - mean) ** 2 for x in raw) / (n - 1))
            if sd is None or abs(sd - want) > 1e-6 + 1e-9 * want:
                v.err(w + ".stddev_ns", "%s, recomputed %.6f (sample deviation)" % (sd, want))
    if cases and len(cases) % 8:
        v.err("cases", "%d cases; expected 8 per pattern" % len(cases))
    if opts.patterns is not None and patterns != opts.patterns:
        v.err("cases", "patterns %s, expected %s" % (patterns, opts.patterns))
    if len(set(patterns)) != len(patterns):
        v.err("cases", "a pattern's cases are not contiguous: %s" % patterns)


def validate(doc, opts, spec=None):
    v = Validator(spec or load_spec())
    meta = doc.get("metadata") if isinstance(doc, dict) else None
    cfg = doc.get("config") if isinstance(doc, dict) else None
    ctx = {
        "pinned": isinstance(meta, dict) and meta.get("pinned_cpu") is not None,
        "input_kind": ((cfg or {}).get("input") or {}).get("kind", "synthetic") if isinstance(cfg, dict) else "synthetic",
    }
    if isinstance(meta, dict):
        aff, pin = meta.get("affinity"), meta.get("pinned_cpu")
        want = aff[0] if isinstance(aff, list) and len(aff) == 1 else None
        if pin != want:
            v.err("metadata.pinned_cpu", "%s, but affinity %s means %s" % (pin, aff, want))
    v.obj(doc, "run", "$", ctx)
    if isinstance(doc, dict):
        semantics(doc, v, opts)
    return v.errors


def markdown(spec):
    rows = ["| Field | Type | Unit | Null allowed | Required | Meaning |", "|---|---|---|---|---|---|"]
    prefix = {"run": "", "metadata": "metadata.", "snapshot": "metadata.before / after.", "thermal": "metadata.*.thermal[].",
              "config": "config.", "input": "config.input.", "case": "cases[].", "lut_access": "cases[].lut_access."}
    for kind, fields in spec["objects"].items():
        for f in fields:
            t = f["type"] + ("<%s>" % f["items"] if f["type"] == "array" else "")
            notes = []
            if f.get("pinned_only"):
                notes.append("null unless pinned to one CPU")
            if f.get("file_only"):
                notes.append("null for synthetic input")
            rows.append("| `%s%s` | %s | %s | %s | %s | %s |" % (
                prefix[kind], f["name"], t, f.get("unit", ""), "yes" if f.get("nullable") else "no",
                "yes" if f.get("required", True) else "no", f["doc"] + ("".join("; " + n for n in notes))))
    return "\n".join(rows) + "\n"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--iterations", type=int)
    ap.add_argument("--warmup", type=int)
    ap.add_argument("--patterns", type=lambda s: s.split(","))
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--input-sha256")
    ap.add_argument("--markdown", action="store_true")
    opts = ap.parse_args(argv)
    spec = load_spec()
    if opts.markdown:
        sys.stdout.write(markdown(spec))
        return 0
    if not opts.files:
        ap.print_usage(sys.stderr)
        return 2
    bad = 0
    for path in opts.files:
        try:
            with open(path, encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            print("%s: %s" % (path, e))
            bad += 1
            continue
        errors = validate(doc, opts, spec)
        for e in errors:
            print("%s: %s" % (path, e))
        print("%s: %s" % (path, "valid" if not errors else "%d error(s)" % len(errors)))
        bad += bool(errors)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
