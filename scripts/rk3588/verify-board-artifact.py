#!/usr/bin/env python3
"""Check a downloaded `RK3588 board checks` artifact before using its results.

  verify-board-artifact.py [--commit SHA] [--json] DIR

DIR is the unzipped artifact (rk3588-board-<stage>-<run>), which holds
metadata.json and the stage's files. Standard library only; reads files, runs
nothing on the board, needs no network.

Checks, each reported as PASS, WARN or FAIL:
  run          metadata.json parses; master ref; the commit (equal to --commit
               when given); stage and expbuf inputs; board identity and the
               preflight's pinned A76/A55 CPUs
  benchmark    both cores' text, JSON and CSV present; exit status 0; the same
               commit everywhere; each JSON pinned to the CPU the preflight chose
               and decoded as the right core; statistics recomputed from raw
               samples (bench-summarize.py); bench-validate.py when available;
               the summary present
  baseline     run conditions (governor, load, frequency change, temperature,
               pinning) from bench-summarize.py: WARN, the numbers are
               indicative only, not a baseline
  probe        evidence and diagnostics archives present and readable; without
               expbuf, no stream or EXPBUF probe in the bundle; with expbuf, the
               probe result present
  privacy      no access tokens or private keys anywhere; no IPv4 or MAC
               addresses inside the archives that are supposed to be redacted

Exit status: 0 no FAIL (WARN allowed), 1 at least one FAIL, 2 usage.
"""
import argparse
import importlib.util
import io
import json
import pathlib
import re
import sys
import tarfile

HERE = pathlib.Path(__file__).resolve().parent
CORES = (("a76", "A76", "Cortex-A76"), ("a55", "A55", "Cortex-A55"))
PATTERNS = ["random", "gradient", "solid"]
SECRET = re.compile(r"\b(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})|-----BEGIN [A-Z ]*PRIVATE KEY-----")
IPV4 = re.compile(r"(?<![\d.])(?!127\.0\.0\.1\b|0\.0\.0\.0\b)(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
MAC = re.compile(r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b")


def load_module(name, filename):
    path = HERE / filename
    if not path.exists():
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Report:
    def __init__(self):
        self.items = []

    def add(self, level, area, msg):
        self.items.append({"level": level, "area": area, "message": msg})

    def ok(self, cond, area, good, bad, level="FAIL"):
        self.add("PASS" if cond else level, area, good if cond else bad)
        return cond

    def failed(self):
        return any(i["level"] == "FAIL" for i in self.items)


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except (OSError, ValueError) as e:
        return None, str(e)


def verify(d, commit=None):
    r = Report()
    summarize = load_module("bench_summarize", "bench-summarize.py")
    validate = load_module("bench_validate", "bench-validate.py")

    meta, err = read_json(d / "metadata.json")
    if not r.ok(meta is not None, "run", "metadata.json parses", "metadata.json missing or invalid: %s" % err):
        return r
    git, inputs, board, runner = meta.get("git") or {}, meta.get("inputs") or {}, meta.get("board") or {}, meta.get("runner") or {}
    sha = git.get("sha") or ""
    r.ok(re.fullmatch(r"[0-9a-f]{40}", sha) is not None, "run", "commit %s" % sha[:12], "metadata has no full commit SHA (%r)" % sha)
    if commit:
        r.ok(sha.startswith(commit.lower()), "run", "commit matches --commit", "commit %s, expected %s" % (sha[:12], commit))
    r.ok(git.get("ref") == "refs/heads/master", "run", "ran from master", "ran from %r, not master" % git.get("ref"))
    stage = inputs.get("stage")
    r.ok(stage in ("tests", "probe"), "run", "stage %s" % stage, "unknown stage %r" % stage)
    expbuf = inputs.get("expbuf") is True
    if expbuf:
        r.add("WARN", "run", "expbuf was enabled: the receiver was opened for buffers; host_idle_confirmed=%s (operator attestation)" % inputs.get("host_idle_confirmed"))
    r.ok("orange pi 5 plus" in (board.get("model") or "").lower(), "run", "board %s" % board.get("model"), "board model %r is not an Orange Pi 5 Plus" % board.get("model"))
    pinned = {"a76": str(runner.get("pinned_a76") or ""), "a55": str(runner.get("pinned_a55") or "")}
    r.ok(all(v.isdigit() for v in pinned.values()), "run", "preflight pinned A76 CPU %s, A55 CPU %s" % (pinned["a76"], pinned["a55"]),
         "preflight didn't record both pinned CPUs (%s)" % pinned)

    # ---------------------------------------------------------------- benchmark
    flags = (d / "cmake-flags.txt").read_text(errors="replace") if (d / "cmake-flags.txt").exists() else ""
    r.ok("benchmark executable: build-codec/bin/DecoderTest" in flags, "benchmark", "build configuration recorded (build-codec/bin/DecoderTest)",
         "cmake-flags.txt missing or without the benchmark executable path")
    docs = []
    for key, label, core in CORES:
        txt_path, json_path, csv_path = (d / ("bgr24-bench-%s.%s" % (key, ext)) for ext in ("txt", "json", "csv"))
        txt = txt_path.read_text(errors="replace") if txt_path.exists() else ""
        if not r.ok(bool(txt), "benchmark", "%s text output present" % label, "%s: %s missing" % (label, txt_path.name)):
            continue
        r.ok("exit status: 0" in txt, "benchmark", "%s exited 0" % label,
             "%s did not exit 0 (%s)" % (label, (re.findall(r"exit status: \d+", txt) or ["no exit status"])[-1]))
        r.ok("commit: %s" % sha in txt, "benchmark", "%s text names commit %s" % (label, sha[:12]), "%s text names another or no commit" % label)
        if "not a baseline" in txt:
            r.add("WARN", "baseline", "%s: load average was above 1.0 before the run" % label)
        doc, err = read_json(json_path)
        if not r.ok(doc is not None, "benchmark", "%s JSON parses" % label, "%s: %s missing or invalid: %s" % (label, json_path.name, err)):
            continue
        docs.append(doc)
        m = doc.get("metadata") or {}
        r.ok(m.get("commit") == sha, "benchmark", "%s JSON commit matches" % label, "%s JSON commit %r != %s" % (label, m.get("commit"), sha[:12]))
        r.ok(m.get("label") == label, "benchmark", "%s JSON label" % label, "%s JSON label %r" % (label, m.get("label")))
        r.ok(str(m.get("pinned_cpu")) == pinned[key], "benchmark", "%s pinned to CPU %s as the preflight chose" % (label, pinned[key]),
             "%s pinned_cpu %r, the preflight chose %s" % (label, m.get("pinned_cpu"), pinned[key]))
        if m.get("core") is None:
            r.add("WARN", "benchmark", "%s: core not decoded (MIDR unreadable); only the label identifies it" % label)
        else:
            r.ok(m.get("core") == core, "benchmark", "%s ran on a %s" % (label, core), "%s ran on a %s, not a %s" % (label, m.get("core"), core))
        if summarize:
            try:
                run = summarize.load(str(json_path))
                r.ok(run["verified"] == len(doc.get("cases", [])) and run["verified"] == 24, "benchmark",
                     "%s: 24 cases, every summary recomputed from raw samples" % label,
                     "%s: %d of %d cases verifiable from raw samples (expected 24, run with --raw)" % (label, run["verified"], len(doc.get("cases", []))))
                notes = summarize.conditions(m)
                r.ok(not notes, "baseline", "%s: clean run conditions" % label, "%s: indicative only, not a baseline: %s" % (label, "; ".join(notes)), level="WARN")
            except summarize.InputError as e:
                r.add("FAIL", "benchmark", "%s: %s" % (label, e))
        if validate:
            class O:
                iterations = warmup = input_sha256 = None
                patterns, raw = PATTERNS, True
            errors = validate.validate(doc, O())
            r.ok(not errors, "benchmark", "%s JSON matches the specification" % label, "%s JSON: %s" % (label, "; ".join(errors[:5])))
        csv = csv_path.read_text(errors="replace").splitlines() if csv_path.exists() else []
        r.ok(len(csv) == 25, "benchmark", "%s CSV: header and 24 rows" % label, "%s CSV has %d lines, expected 25" % (label, len(csv)))
    summary = d / "bgr24-bench-summary.md"
    r.ok(summary.exists() and "# BGR24 benchmark summary" in summary.read_text(errors="replace"), "benchmark", "A76/A55 summary present",
         "bgr24-bench-summary.md missing (runner without python3?): run bench-summarize.py on the JSON", level="WARN")

    # ---------------------------------------------------------------- probe
    archives = {p.name: p for p in d.glob("*.tar.gz")}
    if stage == "probe":
        ev = [p for n, p in archives.items() if n.startswith("rk3588-evidence-")]
        doc_ar = [p for n, p in archives.items() if n.startswith("hyperhdr-doctor-")]
        r.ok(len(ev) == 1, "probe", "evidence archive present", "expected one rk3588-evidence-*.tar.gz, found %d" % len(ev))
        r.ok(len(doc_ar) == 1, "probe", "diagnostics archive present", "expected one hyperhdr-doctor-*.tar.gz, found %d" % len(doc_ar))
        for name in ("evidence-summary.txt", "receiver-signal.txt", "doctor.txt"):
            r.ok((d / name).exists(), "probe", "%s present" % name, "%s missing" % name)
        if ev:
            try:
                with tarfile.open(ev[0]) as t:
                    names = t.getnames()
            except (tarfile.TarError, OSError) as e:
                names = None
                r.add("FAIL", "probe", "%s unreadable: %s" % (ev[0].name, e))
            if names is not None:
                probes = [n for n in names if n.endswith(("/v4l2/receiver/stream.txt", "/v4l2/receiver/expbuf.txt"))]
                if expbuf:
                    r.ok(any(n.endswith("/expbuf.txt") for n in names), "probe", "EXPBUF probe result present (expbuf enabled)",
                         "expbuf was enabled but the bundle has no EXPBUF result")
                else:
                    r.ok(not probes, "probe", "read-only: no stream or EXPBUF probe in the bundle (expbuf disabled)",
                         "expbuf disabled but the bundle contains %s" % ", ".join(probes))
    elif archives:
        r.add("WARN", "probe", "tests stage with archives present: %s" % ", ".join(sorted(archives)))

    # ---------------------------------------------------------------- privacy
    leaks = []
    for p in sorted(d.rglob("*")):
        if p.is_file() and not p.name.endswith(".tar.gz"):
            if SECRET.search(p.read_text(errors="replace")):
                leaks.append(p.name)
    for name, p in sorted(archives.items()):
        try:
            with tarfile.open(p) as t:
                for m in t.getmembers():
                    if not m.isfile():
                        continue
                    text = t.extractfile(m).read().decode("utf-8", "replace")
                    if SECRET.search(text):
                        leaks.append("%s:%s (secret)" % (name, m.name))
                    elif IPV4.search(text) or MAC.search(text):
                        leaks.append("%s:%s (address)" % (name, m.name))
        except (tarfile.TarError, OSError):
            pass
    r.ok(not leaks, "privacy", "no tokens, keys or unredacted addresses found", "review before sharing: %s" % ", ".join(leaks[:10]))
    return r


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--commit")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    d = pathlib.Path(a.dir)
    if not d.is_dir():
        print("verify-board-artifact: %s is not a directory (unzip the artifact first)" % d, file=sys.stderr)
        return 2
    r = verify(d, a.commit)
    if a.json:
        print(json.dumps({"failed": r.failed(), "checks": r.items}, indent=1))
    else:
        for i in r.items:
            print("%-4s %-9s %s" % (i["level"], i["area"], i["message"]))
        n = {lvl: sum(1 for i in r.items if i["level"] == lvl) for lvl in ("PASS", "WARN", "FAIL")}
        print("RESULT: %s (%d pass, %d warn, %d fail)" % ("FAIL" if r.failed() else "OK", n["PASS"], n["WARN"], n["FAIL"]))
    return 1 if r.failed() else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
