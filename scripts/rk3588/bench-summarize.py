#!/usr/bin/env python3
"""Summarize `DecoderTest --bgr24-bench --json` runs (schema hyperhdr.bgr24-bench/1).

Deterministic: the same files in any order give byte-identical output. Standard
library only, so it runs on the board runner and in hosted CI alike.

  bench-summarize.py [--format markdown|csv|json] [--baseline CORE] RUN.json...

What it does
  * validates each file's schema and required fields;
  * when a case carries raw_ns, recomputes min/median/p95/p99/max with the
    benchmark's own rule (sorted[floor(n*p/100)]) and fails on any mismatch;
  * groups cases by core x pattern x range x output x LUT; several runs of one
    group report the median of their medians and the spread between runs;
  * compares cores (e.g. Cortex-A55 vs Cortex-A76), full vs quarter output and
    LUT vs no LUT, as ratios of medians;
  * lists the conditions of every run (load, governor, frequency before/after,
    hottest thermal zone, pinning) so a noisy run isn't mistaken for a baseline.

Missing values are never shown as zero: "n/a" (markdown), "NA" (CSV), null (JSON).
Exit status: 0 ok, 1 invalid input or raw/summary mismatch, 2 usage.
"""
import argparse
import json
import sys

SCHEMA = "hyperhdr.bgr24-bench/1"
STATS = ("min_ns", "median_ns", "p95_ns", "p99_ns", "max_ns")
CASE_FIELDS = ("scenario", "range", "output", "lut", "pattern", "samples", "warmup") + STATS
# Conditions under which a run is not a clean baseline.
LOAD_LIMIT = 1.0
HOT_MILLI_C = 80000


class InputError(Exception):
    pass


def rank(sorted_values, p):
    return sorted_values[min(len(sorted_values) - 1, (len(sorted_values) * p) // 100)]


def recompute(raw):
    s = sorted(raw)
    return {"min_ns": s[0], "median_ns": rank(s, 50), "p95_ns": rank(s, 95), "p99_ns": rank(s, 99), "max_ns": s[-1]}


def median(values):
    """Median of run medians: the middle value, mean of the two middle ones for an even count."""
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def core_of(meta):
    return meta.get("core") or meta.get("label") or (
        "cpu%d" % meta["pinned_cpu"] if meta.get("pinned_cpu") is not None else "unknown")


def conditions(meta):
    """Human-readable notes on run conditions; empty when nothing is off."""
    notes = []
    before, after = meta.get("before") or {}, meta.get("after") or {}
    if meta.get("pinned_cpu") is None:
        notes.append("not pinned to one CPU")
    gov = meta.get("governor")
    if gov is None:
        notes.append("governor unknown")
    elif gov != "performance":
        notes.append("governor %s" % gov)
    load = (before.get("loadavg") or [None])[0]
    if load is None:
        notes.append("load unknown")
    elif load > LOAD_LIMIT:
        notes.append("load %.2f > %.1f" % (load, LOAD_LIMIT))
    fb, fa = before.get("cur_freq_khz"), after.get("cur_freq_khz")
    if fb is None or fa is None:
        notes.append("frequency unknown")
    elif fb != fa:
        notes.append("frequency changed %d -> %d kHz" % (fb, fa))
    temps = [z.get("milli_c") for z in (before.get("thermal") or []) + (after.get("thermal") or []) if z.get("milli_c") is not None]
    if not temps:
        notes.append("temperature unknown")
    elif max(temps) >= HOT_MILLI_C:
        notes.append("hot: %.1f C" % (max(temps) / 1000))
    if not meta.get("commit"):
        notes.append("commit unknown")
    return notes


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise InputError("%s: %s" % (path, e))
    if doc.get("schema") != SCHEMA:
        raise InputError("%s: schema %r, expected %r" % (path, doc.get("schema"), SCHEMA))
    meta, cases = doc.get("metadata"), doc.get("cases")
    if not isinstance(meta, dict) or not isinstance(cases, list) or not cases:
        raise InputError("%s: missing metadata or cases" % path)
    verified = 0
    for c in cases:
        missing = [k for k in CASE_FIELDS if k not in c]
        if missing:
            raise InputError("%s: case %s lacks %s" % (path, c.get("scenario", "?"), ", ".join(missing)))
        raw = c.get("raw_ns")
        if raw is not None:
            if len(raw) != c["samples"]:
                raise InputError("%s: %s has %d raw samples, summary says %d" % (path, c["scenario"], len(raw), c["samples"]))
            got = recompute(raw)
            bad = [k for k in STATS if got[k] != c[k]]
            if bad:
                raise InputError("%s: %s raw samples disagree with %s" % (path, c["scenario"], ", ".join(
                    "%s (file %d, recomputed %d)" % (k, c[k], got[k]) for k in bad)))
            verified += 1
    inp = (doc.get("config") or {}).get("input") or {"kind": "synthetic"}
    return {"path": path, "meta": meta, "cases": cases, "verified": verified, "input": inp}


def summarize(runs, baseline=None):
    runs = sorted(runs, key=lambda r: (core_of(r["meta"]), r["meta"].get("commit") or "", r["path"]))
    groups = {}
    for r in runs:
        core = core_of(r["meta"])
        for c in r["cases"]:
            key = (core, c["pattern"], c["range"], c["output"], bool(c["lut"]))
            groups.setdefault(key, []).append(c)
    rows = []
    for key in sorted(groups):
        cs = groups[key]
        medians = [c["median_ns"] for c in cs]
        rows.append({
            "core": key[0], "pattern": key[1], "range": key[2], "output": key[3], "lut": key[4],
            "runs": len(cs), "median_ns": median(medians),
            "p95_ns": median([c["p95_ns"] for c in cs]),
            "spread_pct": (100.0 * (max(medians) - min(medians)) / median(medians)) if len(cs) > 1 and median(medians) else None,
            "lut_lines_64": cs[0]["lut_access"]["distinct_lines_64"] if cs[0].get("lut_access") else None,
        })
    by = {(r["core"], r["pattern"], r["range"], r["output"], r["lut"]): r for r in rows}
    cores = sorted({r["core"] for r in rows})
    # Default baseline: the fastest core over all its groups (ties by name), so
    # ratios read as "how many times slower than the fast core".
    total = {c: sum(r["median_ns"] for r in rows if r["core"] == c) for c in cores}
    base = baseline if baseline in cores else (min(cores, key=lambda c: (total[c], c)) if cores else None)
    if baseline and baseline not in cores:
        raise InputError("baseline core %r not in runs (%s)" % (baseline, ", ".join(cores)))

    def ratio(a, b):
        return a["median_ns"] / b["median_ns"] if a and b and b["median_ns"] else None

    core_cmp, scale_cmp, lut_cmp = [], [], []
    for (core, pattern, rng, out, lut), r in sorted(by.items()):
        if core != base and (base, pattern, rng, out, lut) in by:
            core_cmp.append({"core": core, "baseline": base, "pattern": pattern, "range": rng, "output": out, "lut": lut,
                             "ratio": ratio(r, by[(base, pattern, rng, out, lut)])})
        if out == "full" and (core, pattern, rng, "quarter", lut) in by:
            scale_cmp.append({"core": core, "pattern": pattern, "range": rng, "lut": lut,
                              "full_over_quarter": ratio(r, by[(core, pattern, rng, "quarter", lut)])})
        if lut and (core, pattern, rng, out, False) in by:
            lut_cmp.append({"core": core, "pattern": pattern, "range": rng, "output": out,
                            "lut_over_nolut": ratio(r, by[(core, pattern, rng, out, False)])})
    run_info = [{"file": r["path"], "core": core_of(r["meta"]), "label": r["meta"].get("label"),
                 "commit": r["meta"].get("commit"), "pinned_cpu": r["meta"].get("pinned_cpu"),
                 "cases": len(r["cases"]), "raw_verified": r["verified"], "conditions": conditions(r["meta"])} for r in runs]
    # Saved raw frames (DecoderTest --input): decoder timing on bytes already in
    # memory. Listed separately so they can't be read as live-capture results.
    saved = []
    for r in runs:
        inp = r.get("input") or {}
        if inp.get("kind") == "file":
            saved.append({"pattern": "file:" + (inp.get("sha256") or "")[:12], "name": inp.get("name"), "sha256": inp.get("sha256"),
                          "declared_range": inp.get("declared_range"), "file": r["path"]})
    return {"schema": "hyperhdr.bgr24-bench-summary/1", "baseline_core": base, "runs": run_info, "rows": rows,
            "saved_frame_inputs": saved,
            "core_comparison": core_cmp, "full_vs_quarter": scale_cmp, "lut_cost": lut_cmp}


def ms(ns):
    return "n/a" if ns is None else "%.3f" % (ns / 1e6)


def x(v):
    return "n/a" if v is None else "%.2fx" % v


def markdown(s):
    o = ["# BGR24 benchmark summary", "",
         "Medians of per-run medians (ms). Ratios compare medians. Conditions marked below mean a run is not a clean baseline.", "",
         "## Runs", "", "| File | Core | Commit | CPU | Cases (raw-verified) | Conditions |", "|---|---|---|---|---|---|"]
    for r in s["runs"]:
        o.append("| %s | %s | %s | %s | %d (%d) | %s |" % (r["file"], r["core"], (r["commit"] or "n/a")[:12],
                 "n/a" if r["pinned_cpu"] is None else r["pinned_cpu"], r["cases"], r["raw_verified"],
                 "; ".join(r["conditions"]) or "none"))
    if s.get("saved_frame_inputs"):
        o += ["", "## Saved-frame inputs", "",
              "Patterns `file:<hash>` are decoder timings on a saved raw frame already in memory (`DecoderTest --input`). "
              "**They are not live-capture measurements**: no V4L2 dequeue, DMA, driver or HDMI timing is included.", "",
              "| Pattern | File | SHA-256 | Declared range | Run |", "|---|---|---|---|---|"]
        o += ["| %s | %s | %s | %s | %s |" % (i["pattern"], i["name"], i["sha256"], i["declared_range"], i["file"]) for i in s["saved_frame_inputs"]]
    o += ["", "## Results", "", "| Core | Pattern | Range | Output | LUT | Runs | Median | P95 | Run spread | LUT lines (64 B) |",
          "|---|---|---|---|---|---:|---:|---:|---:|---:|"]
    for r in s["rows"]:
        o.append("| %s | %s | %s | %s | %s | %d | %s | %s | %s | %s |" % (
            r["core"], r["pattern"] + (" (saved frame)" if r["pattern"].startswith("file:") else ""), r["range"], r["output"], "yes" if r["lut"] else "no", r["runs"], ms(r["median_ns"]),
            ms(r["p95_ns"]), "n/a" if r["spread_pct"] is None else "%.1f%%" % r["spread_pct"],
            "n/a" if r["lut_lines_64"] is None else r["lut_lines_64"]))
    if s["core_comparison"]:
        o += ["", "## Core comparison (core / %s)" % s["baseline_core"], "",
              "| Core | Pattern | Range | Output | LUT | Ratio |", "|---|---|---|---|---|---:|"]
        o += ["| %s | %s | %s | %s | %s | %s |" % (c["core"], c["pattern"], c["range"], c["output"], "yes" if c["lut"] else "no", x(c["ratio"]))
              for c in s["core_comparison"]]
    o += ["", "## Full vs quarter output (full / quarter)", "", "| Core | Pattern | Range | LUT | Ratio |", "|---|---|---|---|---:|"]
    o += ["| %s | %s | %s | %s | %s |" % (c["core"], c["pattern"], c["range"], "yes" if c["lut"] else "no", x(c["full_over_quarter"]))
          for c in s["full_vs_quarter"]]
    o += ["", "## LUT cost (LUT / no LUT)", "", "| Core | Pattern | Range | Output | Ratio |", "|---|---|---|---|---:|"]
    o += ["| %s | %s | %s | %s | %s |" % (c["core"], c["pattern"], c["range"], c["output"], x(c["lut_over_nolut"]))
          for c in s["lut_cost"]]
    return "\n".join(o) + "\n"


def csv(s):
    def f(v):
        return "NA" if v is None else ("%.6f" % v if isinstance(v, float) else str(v))
    o = ["core,pattern,range,output,lut,runs,median_ns,p95_ns,spread_pct,lut_lines_64"]
    for r in s["rows"]:
        o.append(",".join(f(r[k]) if k != "lut" else ("yes" if r[k] else "no") for k in
                          ("core", "pattern", "range", "output", "lut", "runs", "median_ns", "p95_ns", "spread_pct", "lut_lines_64")))
    return "\n".join(o) + "\n"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--format", choices=("markdown", "csv", "json"), default="markdown")
    ap.add_argument("--baseline", help="core to compare others against (default: the fastest core)")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    try:
        s = summarize([load(p) for p in a.files], a.baseline)
    except InputError as e:
        print("bench-summarize: %s" % e, file=sys.stderr)
        return 1
    if a.format == "json":
        sys.stdout.write(json.dumps(s, indent=1, sort_keys=True) + "\n")
    elif a.format == "csv":
        sys.stdout.write(csv(s))
    else:
        sys.stdout.write(markdown(s))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
