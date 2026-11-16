#!/usr/bin/env python3
"""Fixture tests for scripts/rk3588/verify-board-artifact.py.

  verify-board-artifact-test.py SAMPLE_DIR

SAMPLE_DIR holds the synthetic A76/A55 runs from `bench-report-test --emit-sample`
(the real C++ writer). The test builds a complete board artifact around them,
for both stages, and then breaks it one way at a time: every defect must be a
FAIL (or a WARN where the numbers stay usable but aren't a baseline).
"""
import copy
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/rk3588/verify-board-artifact.py"
SHA = "0123456789abcdef0123456789abcdef01234567"
failures = 0


def check(cond, msg):
    global failures
    print(("PASS " if cond else "FAIL ") + msg)
    failures += 0 if cond else 1


def three_patterns(doc):
    """The board runs random,gradient,solid: add solid cases (copies of gradient)."""
    d = copy.deepcopy(doc)
    solid = [dict(c, pattern="solid", scenario=c["scenario"].replace("-gradient", "-solid")) for c in d["cases"] if c["pattern"] == "gradient"]
    d["cases"] += solid
    d["metadata"]["commit"] = SHA
    return d


def tar_gz(path, root, files):
    with tarfile.open(path, "w:gz") as t:
        for name, text in files.items():
            data = text.encode()
            info = tarfile.TarInfo("%s/%s" % (root, name))
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))


def artifact(base, sample, stage="tests", expbuf=False, evidence_extra=None):
    d = base / ("artifact-%s" % stage)
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    (d / "metadata.json").write_text(json.dumps({
        "schema": 1, "generated": "2026-11-16T12:00:00Z",
        "git": {"sha": SHA, "ref": "refs/heads/master", "run_id": "1", "run_attempt": "1"},
        "inputs": {"stage": stage, "expbuf": expbuf, "host_idle_confirmed": expbuf},
        "board": {"model": "Xunlong Orange Pi 5 Plus", "compatible": "xunlong,orangepi-5-plus", "a55": "0 1 2 3", "a76": "4 5 6 7",
                  "receiver": "/dev/video3", "receiver_driver": "snps_hdmirx"},
        "runner": {"container": True, "allowed_cpus": "0-7", "pinned_a55": "0", "pinned_a76": "4"},
        "os": {}, "toolchain": {}}))
    (d / "cmake-flags.txt").write_text("codec_test: cmake -DPLATFORM=linux\nbenchmark executable: build-codec/bin/DecoderTest\n")
    for key, label in (("a76", "A76"), ("a55", "A55")):
        doc = three_patterns(json.loads((sample / ("bgr24-bench-%s.json" % key)).read_text()))
        (d / ("bgr24-bench-%s.json" % key)).write_text(json.dumps(doc))
        (d / ("bgr24-bench-%s.txt" % key)).write_text("== %s, CPU %s\ncommit: %s\n-- benchmark\n...\nexit status: 0\n" % (label, doc["metadata"]["pinned_cpu"], SHA))
        (d / ("bgr24-bench-%s.csv" % key)).write_text("schema,commit\n" + "".join("x,%s\n" % SHA for _ in range(24)))
    (d / "bgr24-bench-summary.md").write_text("# BGR24 benchmark summary\n")
    if stage == "probe":
        files = {"summary.txt": "receiver: /dev/video3\nhost <host> at <ipv4> mac <mac>\n"}
        if expbuf:
            files["v4l2/receiver/expbuf.txt"] = "open=ok\nreqbufs=4\nexpbuf=ok\n"
        files.update(evidence_extra or {})
        tar_gz(d / "rk3588-evidence-20261116T120000Z.tar.gz", "rk3588-evidence-20261116T120000Z", files)
        tar_gz(d / "hyperhdr-doctor-20261116T120001Z.tar.gz", "hyperhdr-doctor-20261116T120001Z", {"report.txt": "service ok on <host>\n"})
        for name in ("evidence-summary.txt", "receiver-signal.txt", "doctor.txt"):
            (d / name).write_text("ok\n")
    return d


def run(d, *extra):
    return subprocess.run([sys.executable, str(SCRIPT), *extra, str(d)], capture_output=True, text=True)


def edit_json(path, fn):
    doc = json.loads(path.read_text())
    fn(doc)
    path.write_text(json.dumps(doc))


sample = pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory() as tmp:
    base = pathlib.Path(tmp)

    good = artifact(base, sample)
    r = run(good)
    check(r.returncode == 0 and "RESULT: OK" in r.stdout and " 0 fail" in r.stdout, "complete tests-stage artifact: OK")
    check("WARN baseline  A55: indicative only, not a baseline: governor unknown" in r.stdout,
          "A55 sample without a governor: WARN not a baseline (the results stay usable)")
    check("PASS benchmark A76: 24 cases, every summary recomputed from raw samples" in r.stdout, "raw samples recomputed for 24 cases")
    check(run(good, "--commit", SHA[:12]).returncode == 0, "--commit prefix matches")
    r = run(good, "--commit", "fedcba987654")
    check(r.returncode == 1 and "expected fedcba987654" in r.stdout, "--commit mismatch: FAIL")
    js = json.loads(run(good, "--json").stdout)
    check(js["failed"] is False and all({"level", "area", "message"} <= set(c) for c in js["checks"]), "--json output")

    def broken(desc, mutate, expect, stage="tests", expbuf=False, evidence_extra=None, code=1):
        d = artifact(base, sample, stage, expbuf, evidence_extra)
        mutate(d)
        r = run(d)
        ok = r.returncode == code and expect in r.stdout
        check(ok, desc)
        if not ok:
            print("     " + "\n     ".join(l for l in r.stdout.splitlines() if not l.startswith("PASS")))

    broken("metadata.json missing: FAIL", lambda d: (d / "metadata.json").unlink(), "metadata.json missing or invalid")
    broken("not from master: FAIL", lambda d: edit_json(d / "metadata.json", lambda m: m["git"].update(ref="refs/heads/feature")), "not master")
    broken("another board: FAIL", lambda d: edit_json(d / "metadata.json", lambda m: m["board"].update(model="Radxa ROCK 5B")), "is not an Orange Pi 5 Plus")
    broken("benchmark exit status 3: FAIL", lambda d: (d / "bgr24-bench-a76.txt").write_text("commit: %s\nexit status: 3\n" % SHA), "A76 did not exit 0 (exit status: 3)")
    broken("A55 JSON missing: FAIL", lambda d: (d / "bgr24-bench-a55.json").unlink(), "A55: bgr24-bench-a55.json missing")
    broken("JSON from another commit: FAIL", lambda d: edit_json(d / "bgr24-bench-a76.json", lambda j: j["metadata"].update(commit="f" * 40)), "A76 JSON commit")
    broken("benchmark pinned to another CPU than the preflight chose: FAIL",
           lambda d: edit_json(d / "bgr24-bench-a76.json", lambda j: j["metadata"].update(pinned_cpu=5)), "A76 pinned_cpu 5, the preflight chose 4")
    broken("A55 label on a Cortex-A76: FAIL", lambda d: edit_json(d / "bgr24-bench-a55.json", lambda j: j["metadata"].update(core="Cortex-A76")),
           "A55 ran on a Cortex-A76, not a Cortex-A55")
    broken("summary that raw samples don't give: FAIL",
           lambda d: edit_json(d / "bgr24-bench-a76.json", lambda j: j["cases"][0].update(median_ns=j["cases"][0]["median_ns"] + 1)),
           "raw samples disagree with median_ns")
    broken("run without --raw: FAIL", lambda d: edit_json(d / "bgr24-bench-a76.json", lambda j: [c.pop("raw_ns") for c in j["cases"]]),
           "0 of 24 cases verifiable")
    broken("CSV truncated: FAIL", lambda d: (d / "bgr24-bench-a55.csv").write_text("schema\nx\n"), "A55 CSV has 2 lines, expected 25")
    broken("summary missing: WARN only", lambda d: (d / "bgr24-bench-summary.md").unlink(), "WARN benchmark bgr24-bench-summary.md missing", code=0)
    broken("powersave governor and high load: WARN only",
           lambda d: edit_json(d / "bgr24-bench-a76.json", lambda j: (j["metadata"].update(governor="powersave"), j["metadata"]["before"].update(loadavg=[2.5, 2.0, 1.5]))),
           "A76: indicative only, not a baseline: governor powersave; load 2.50 > 1.0", code=0)
    broken("access token in a result file: FAIL", lambda d: (d / "cmake-flags.txt").write_text("benchmark executable: build-codec/bin/DecoderTest\nGH=ghp_" + "a" * 36 + "\n"),
           "review before sharing: cmake-flags.txt")

    # probe stage
    good_probe = artifact(base, sample, "probe")
    r = run(good_probe)
    check(r.returncode == 0 and "read-only: no stream or EXPBUF probe in the bundle (expbuf disabled)" in r.stdout, "probe stage, expbuf off: OK and read-only")
    r = run(artifact(base, sample, "probe", expbuf=True))
    check(r.returncode == 0 and "EXPBUF probe result present" in r.stdout and "WARN run" in r.stdout, "probe with expbuf: OK, with a WARN naming the attestation")
    broken("expbuf disabled but a stream probe in the bundle: FAIL", lambda d: None, "expbuf disabled but the bundle contains",
           stage="probe", evidence_extra={"v4l2/receiver/stream.txt": "frames\n"})
    broken("evidence archive missing: FAIL", lambda d: [p.unlink() for p in d.glob("rk3588-evidence-*.tar.gz")], "expected one rk3588-evidence-*.tar.gz, found 0", stage="probe")
    broken("corrupt evidence archive: FAIL", lambda d: [p.write_bytes(b"not a tar") for p in d.glob("rk3588-evidence-*.tar.gz")], "unreadable", stage="probe")
    broken("unredacted address in the evidence bundle: FAIL", lambda d: None, "review before sharing: rk3588-evidence-",
           stage="probe", evidence_extra={"network.txt": "wled at 192.168.1.40\n"})
    broken("receiver-signal.txt missing: FAIL", lambda d: (d / "receiver-signal.txt").unlink(), "receiver-signal.txt missing", stage="probe")

    r = subprocess.run([sys.executable, str(SCRIPT), str(base / "nope")], capture_output=True, text=True)
    check(r.returncode == 2 and "unzip the artifact first" in r.stderr, "not a directory: exit 2")

print("%d failure(s)" % failures if failures else "all board-artifact checks passed")
sys.exit(1 if failures else 0)
