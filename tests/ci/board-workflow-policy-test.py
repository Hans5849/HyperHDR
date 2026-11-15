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
      *) mkdir -p "$B/bin"; printf '#!/bin/sh\necho "bench ok"\nexit ${BENCH_EXIT:-0}\n' >"$B/bin/DecoderTest"
         [ "$BENCH_MODE" = noexec ] || chmod +x "$B/bin/DecoderTest";; esac;;
esac
""")
        (stubs / "taskset").write_text('#!/usr/bin/env bash\n[ "$1" = -cp ] && { echo "pid current affinity list: 0-7"; exit 0; }\nshift 2; exec "$@"\n')
        for f in stubs.iterdir(): f.chmod(f.stat().st_mode | stat.S_IXUSR)
        env = {"PATH": f"{stubs}:{os.environ['PATH']}", "GITHUB_SHA": "abc123", "A76_CPU": "4", "A55_CPU": "0", "BENCH_MODE": mode}
        env.update(env_extra or {})
        r = subprocess.run(["bash", "-c", script], cwd=d, env=env, capture_output=True, text=True)
        files = {f.name: f.read_text() for f in (d / "board-results").glob("*")} if (d / "board-results").exists() else {}
        return r, files

r, files = run_bench("good")
check(r.returncode == 0 and set(files) >= {"bgr24-bench-a76.txt", "bgr24-bench-a55.txt", "cmake-flags.txt"}
      and all("bench ok" in files[f] and "exit status: 0" in files[f] and "commit: abc123" in files[f] for f in ("bgr24-bench-a76.txt", "bgr24-bench-a55.txt")),
      "benchmark step succeeds and records both cores, the commit and the exit status")
check("benchmark executable: build-codec/bin/DecoderTest" in files.get("cmake-flags.txt", ""), "build configuration records the executable path")
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
