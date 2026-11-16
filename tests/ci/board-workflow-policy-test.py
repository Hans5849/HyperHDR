#!/usr/bin/env python3
"""Trust-boundary checks for .github/workflows/rk3588-board.yml (issue #28).

The board runner is a real device in someone's home. These invariants keep
untrusted code and accidental runs off it:
- manual dispatch is the only trigger (no pull_request, push or schedule)
- master only, through the rk3588-hil environment (reviewers can gate runs)
- the job needs the Orange Pi-specific labels, not just "rk3588"
- read-only token, preflight first, no sudo, no service control
- the benchmark runs the executable CMake really builds (issue #28: the first
  board run died with exit 127 on ./build-codec/DecoderTest), fails loudly and
  is exercised here with stubs, not only matched as text
"""
import os, pathlib, re, stat, subprocess, sys, tempfile
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
path = ROOT / ".github/workflows/rk3588-board.yml"
wf = yaml.safe_load(path.read_text())
on = wf.get(True, wf.get("on"))   # PyYAML reads the "on" key as True
failures = []
def check(cond, msg):
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond: failures.append(msg)

check(list(on) == ["workflow_dispatch"], f"only workflow_dispatch triggers it (found {list(on)})")
check(wf.get("permissions") == {"contents": "read"}, "token is contents: read")
jobs = wf["jobs"]
for name, job in jobs.items():
    labels = job.get("runs-on", [])
    labels = [labels] if isinstance(labels, str) else labels
    if "self-hosted" not in labels:
        continue
    check({"orange-pi-5-plus", "hyperhdr-hil"} <= set(labels), f"{name}: targets the Orange Pi labels {labels}")
    cond = job.get("if", "")
    # Evaluate the condition instead of matching text, so "|| true" or "!=" can't pass.
    def gate(expr, ctx):
        terms = [t.strip() for t in str(expr).split("&&")]
        for t in terms:
            m = re.fullmatch(r"(github\.(?:repository|ref))\s*==\s*'([^']+)'", t)
            if not m: raise ValueError(f"unsupported term {t!r}")
            if ctx[m.group(1)] != m.group(2): return False
        return True
    master = {"github.repository": "Hans5849/HyperHDR", "github.ref": "refs/heads/master"}
    try:
        ok_gate = (gate(cond, master)
                   and not gate(cond, {**master, "github.repository": "someone/HyperHDR"})
                   and not gate(cond, {**master, "github.ref": "refs/heads/claude/rk3588-hil-runner"})
                   and not gate(cond, {**master, "github.ref": "refs/pull/30/merge"}))
    except ValueError as e:
        ok_gate = False; cond = f"{cond} [{e}]"
    check(ok_gate, f"{name}: runs for master of Hans5849/HyperHDR and for nothing else ({cond})")
    check("continue-on-error" not in job and not any("continue-on-error" in s for s in job.get("steps", [])),
          f"{name}: no continue-on-error masking failures")
    check("container" not in job and "services" not in job, f"{name}: no job container or service containers (no --privileged, no Docker socket)")
    check(job.get("permissions") in (None, {"contents": "read"}), f"{name}: job token stays contents: read")
    check(job.get("environment") == "rk3588-hil", f"{name}: runs in the rk3588-hil environment")
    steps = job.get("steps", [])
    runs = [s for s in steps if "run" in s]
    check(bool(runs) and "board-preflight.sh" in runs[0]["run"], f"{name}: preflight is the first command step")
    script = "\n".join(s["run"] for s in runs)
    check(not re.search(r"(^|[\s;&|(])sudo\s", script), f"{name}: no sudo")
    check(not re.search(r"systemctl\s+(stop|start|restart|disable|enable|kill)|\bpkill\b|\bkill\s", script),
          f"{name}: never controls services or processes")
    check(not re.search(r"(^|\s)(apt|apt-get|dpkg|usermod|chown)\s", script), f"{name}: never changes packages or users")
    check(not re.search(r"\bdocker\b|/var/run/docker|--privileged|\bsecrets\.", script), f"{name}: no docker, privileged mode or secrets")
    check(not any("${{" in s["run"] for s in runs), f"{name}: no expression interpolated into a shell script (pass inputs through env:)")
    check(all(s.get("shell") == "bash" for s in runs if "| tee" in s["run"]), f"{name}: every step that pipes into tee uses shell: bash (pipefail)")
    check(on["workflow_dispatch"]["inputs"]["expbuf"]["default"] is False
          and on["workflow_dispatch"]["inputs"]["host_idle_confirmed"]["default"] is False
          and on["workflow_dispatch"]["inputs"]["stage"]["default"] == "tests", f"{name}: EXPBUF, probe and the host-idle attestation stay opt-in (defaults: tests, false, false)")
    for s in steps:
        if "collect-evidence.sh" in s.get("run", "") or "hyperhdr-doctor.sh" in s.get("run", ""):
            check("inputs.stage == 'probe'" in s.get("if", ""), f"{name}: step '{s.get('name')}' runs only in the probe stage")
    uses = [s["uses"] for s in steps if "uses" in s]
    check(all(re.match(r"actions/(checkout|upload-artifact)@v\d", u) for u in uses), f"{name}: only first-party actions ({uses})")

# --- the benchmark step: right executable, loud failures ---------------------------
cmake = (ROOT / "tests/codec_test/CMakeLists.txt").read_text()
m = re.search(r"SET\(EXECUTABLE_OUTPUT_PATH\s+\$\{PROJECT_BINARY_DIR\}/(\w+)\)", cmake, re.I)
check(bool(m) and "add_executable(DecoderTest" in cmake, "CMake puts DecoderTest in ${PROJECT_BINARY_DIR}/<dir>")
expected = f"build-codec/{m.group(1) if m else '?'}/DecoderTest"
bench = [s for s in wf["jobs"]["board"]["steps"] if "BGR24 decode benchmark" in s.get("name", "")]
# The board runs every hosted sanitizer suite natively, including the independent
# BGR24 reference suite, and cleans each build tree afterwards.
board_steps = wf["jobs"]["board"]["steps"]
board_runs = "\n".join(s.get("run", "") for s in board_steps)
for suite, build in (("tests/v4l2", "build-v4l2"), ("tests/bgr24", "build-bgr24"), ("tests/cli", "build-cli"), ("tests/codec_test", "build-codec-asan")):
    check(f"cmake -S {suite} -B {build}" in board_runs and f"ctest --test-dir {build}" in board_runs, f"board runs {suite} ({build})")
    check(re.search(r"rm -rf .*\b%s\b" % re.escape(build), board_runs) is not None, f"board cleans {build}")
check("fsanitize=address,undefined" in next((s["run"] for s in board_steps if "tests/bgr24" in s.get("run", "")), ""), "BGR24 reference suite runs under ASan/UBSan")
# Routine CI is GitHub-hosted: no other workflow may target a self-hosted runner.
for wf_path in sorted((ROOT / ".github/workflows").glob("*.y*ml")):
    if wf_path.name == "rk3588-board.yml":
        continue
    runs_on = re.findall(r"runs-on:.*", wf_path.read_text())
    check(bool(runs_on) and not any(re.search(r"self-hosted|hyperhdr-hil|orange-pi|rk3588", l) for l in runs_on),
          f"{wf_path.name}: GitHub-hosted runners only")
# The board is a hardware resource, not general CI (#28, owner intent
# 2026-11-15): generic suites stay on hosted runners, including hosted ARM64.
# Every C++ suite the board builds must also run in hosted CI, so the board is
# never the only place a test runs.
hosted = (ROOT / ".github/workflows/push-pull.yml").read_text()
board_suites = set(re.findall(r"cmake -S (tests/[a-z_0-9]+)", board_runs))
check(board_suites <= set(re.findall(r"cmake -S (tests/[a-z_0-9]+)", hosted)), f"every board suite also runs hosted ({sorted(board_suites)})")
check(re.search(r"runner: ubuntu-[0-9.]+-arm\b", hosted) is not None, "hosted CI has a native ARM64 runner for the hardware-free suites")
verify = [s for s in board_steps if "verify-board-artifact.py" in s.get("run", "")]
check(len(verify) == 1 and verify[0].get("if") == "always()" and "--commit \"$GITHUB_SHA\" board-results" in verify[0]["run"],
      "the artifact is verified in place, always, against this run's commit")
names = [s.get("name", "") for s in board_steps]
check(names.index(verify[0]["name"]) < names.index("Upload results") if verify else False, "verification output is uploaded with the artifact")
check(len(bench) == 1, "one BGR24 benchmark step")
script = bench[0]["run"]
check(re.search(r'^\s*BUILD=build-codec\s*$', script, re.M) and re.search(r'^\s*BENCH="\$BUILD/%s/DecoderTest"\s*$' % (m.group(1) if m else "?"), script, re.M),
      f"benchmark executable is {expected}, as CMake builds it")
check("./build-codec/DecoderTest" not in "\n".join(s.get("run", "") for s in wf["jobs"]["board"]["steps"] if "ctest" not in s.get("run", "")),
      "no step runs ./build-codec/DecoderTest (the path that failed on the board)")
check(all('"$BENCH"' in l for l in script.splitlines() if re.search(r"\btaskset -c\b", l)), "every pinned run uses $BENCH")
check("tee" not in script and "set -euo pipefail" in script, "benchmark output isn't piped through tee and the step stops on errors")
check('[ -x "$BENCH" ]' in script or '! -x "$BENCH"' in script, "the executable is checked before benchmarking")

def run_bench(mode, env_extra=None):
    with tempfile.TemporaryDirectory() as d:
        d = pathlib.Path(d); stubs = d / "stubs"; stubs.mkdir()
        (stubs / "cmake").write_text("""#!/usr/bin/env bash
case "$1" in
  -S) while [ $# -gt 0 ]; do [ "$1" = -B ] && B=$2; shift; done; mkdir -p "$B"; echo 'CMAKE_BUILD_TYPE:STRING=Release' >"$B/CMakeCache.txt";;
  --build) B=$2; case "$BENCH_MODE" in
      wrongpath) printf '#!/bin/sh\necho ok\n' >"$B/DecoderTest"; chmod +x "$B/DecoderTest";;
      *) mkdir -p "$B/bin"; cp "$STUB_BENCH" "$B/bin/DecoderTest"
         if [ "$BENCH_MODE" = noexec ]; then chmod -x "$B/bin/DecoderTest"; else chmod +x "$B/bin/DecoderTest"; fi;; esac;;
esac
""")
        (stubs / "taskset").write_text('#!/usr/bin/env bash\n[ "$1" = -cp ] && { echo "pid current affinity list: 0-7"; exit 0; }\nshift 2; exec "$@"\n')
        # Stub benchmark: prints the table line, and (unless BENCH_NOJSON) writes a
        # minimal valid JSON run to the --json path, like the real DecoderTest.
        (d / "bench-stub").write_text(r"""#!/usr/bin/env bash
echo "bench ok"; echo "args: $*"
while [ $# -gt 0 ]; do case "$1" in --json) J=$2; shift;; --label) L=$2; shift;; esac; shift; done
if [ -n "$J" ] && [ -z "$BENCH_NOJSON" ]; then
  printf '{"schema":"hyperhdr.bgr24-bench/1","metadata":{"label":"%s","commit":"abc123","pinned_cpu":4,"before":{},"after":{}},"cases":[{"scenario":"full-full-nolut-random","range":"full","output":"full","lut":false,"pattern":"random","samples":1,"warmup":1,"min_ns":5,"median_ns":5,"p95_ns":5,"p99_ns":5,"max_ns":5,"raw_ns":[5]}]}' "$L" >"$J"
fi
exit ${BENCH_EXIT:-0}
""")
        (d / "bench-stub").chmod(0o755)
        for f in stubs.iterdir(): f.chmod(f.stat().st_mode | stat.S_IXUSR)
        env = {"PATH": f"{stubs}:{os.environ['PATH']}", "GITHUB_SHA": "abc123", "A76_CPU": "4", "A55_CPU": "0", "BENCH_MODE": mode,
               "STUB_BENCH": str(d / "bench-stub")}
        env.update(env_extra or {})
        (d / "scripts").symlink_to(ROOT / "scripts")   # the step runs from the checkout root
        r = subprocess.run(["bash", "-c", script], cwd=d, env=env, capture_output=True, text=True)
        files = {f.name: f.read_text() for f in (d / "board-results").glob("*")} if (d / "board-results").exists() else {}
        return r, files

r, files = run_bench("good")
check(r.returncode == 0 and set(files) >= {"bgr24-bench-a76.txt", "bgr24-bench-a55.txt", "cmake-flags.txt"}
      and all("bench ok" in files[f] and "exit status: 0" in files[f] and "commit: abc123" in files[f] for f in ("bgr24-bench-a76.txt", "bgr24-bench-a55.txt")),
      "benchmark step succeeds and records both cores, the commit and the exit status")
check("benchmark executable: build-codec/bin/DecoderTest" in files.get("cmake-flags.txt", ""), "build configuration records the executable path")
a76 = files.get("bgr24-bench-a76.txt", "")
check("--pattern random,gradient,solid --raw" in a76 and "--json board-results/bgr24-bench-a76.json" in a76
      and "--csv board-results/bgr24-bench-a76.csv" in a76 and "--commit abc123 --label A76" in a76,
      "each core's run asks for raw JSON and CSV with the commit and core label")
check({"bgr24-bench-a76.json", "bgr24-bench-a55.json"} <= set(files), "machine-readable results are kept for both cores")
summary = files.get("bgr24-bench-summary.md", "")
check("# BGR24 benchmark summary" in summary and "| A76 |" in summary and "| A55 |" in summary and "1 (1)" in summary,
      "the summary covers both cores and verified the raw samples")
r, files = run_bench("good", {"BENCH_NOJSON": "1"})
check(r.returncode != 0 and "wrote no board-results/bgr24-bench-a76.json" in files.get("bgr24-bench-a76.txt", ""),
      "a benchmark that exits 0 without its JSON fails the step")
r, files = run_bench("wrongpath")
check(r.returncode != 0 and "missing or not executable" in r.stdout and "bgr24-bench-a76.txt" not in files,
      "binary outside bin/ (the failure seen on the board) fails before any benchmark runs")
r, files = run_bench("noexec")
check(r.returncode != 0 and "missing or not executable" in r.stdout, "a non-executable benchmark binary fails")
r, files = run_bench("failbench", {"BENCH_EXIT": "3"})
check(r.returncode != 0 and "exit status: 3" in files.get("bgr24-bench-a76.txt", "") and "exit status: 3" in files.get("bgr24-bench-a55.txt", ""),
      "a failing benchmark fails the step, and both cores are still attempted and recorded")
r, files = run_bench("good", {"A76_CPU": ""})
check(r.returncode != 0 and "did not export A76_CPU" in r.stderr, "missing preflight output fails instead of pinning blindly")

print(f"{len(failures)} failure(s)" if failures else "all board-workflow policy checks passed")
sys.exit(1 if failures else 0)
