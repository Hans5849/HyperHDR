#!/usr/bin/env python3
"""CI scope checks for this fork: Linux-only CI, with the RK3588 release gates kept.

Owner decision (2026-11-21, issue #81): Windows and macOS are outside the supported
scope of Hans5849/HyperHDR, so no workflow may run a Windows or macOS job or sign
Windows installers. This test also guards against upstream synchronization
re-introducing automatic multi-distribution matrices or ungated upstream
publishing, and it runs from the fork-only fork-guard.yml as well as push-pull.yml. These checks also keep the required Linux testing from being lost:
- the sanitizer suites run on x86_64 and arm64, with every required suite present;
- CodeQL and the repository-policy tests stay in routine CI;
- releases still need policy, sanitizer, CodeQL and Debian 13 package jobs;
- every `needs` points at a job that exists (no dangling dependency);
- release validation and master runs are never cancelled;
- no workflow uses pull_request_target.
"""
import importlib.util
import pathlib
import sys

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
WF_DIR = ROOT / ".github/workflows"
failures = 0


def check(cond, msg):
    global failures
    print(("PASS " if cond else "FAIL ") + msg)
    failures += 0 if cond else 1


def load(path):
    wf = yaml.safe_load(path.read_text())
    wf["on"] = wf.get(True, wf.get("on"))   # PyYAML reads the "on" key as True
    return wf


workflows = {p.name: load(p) for p in sorted(WF_DIR.glob("*.yml"))}

# --- no Windows or macOS anywhere ------------------------------------------------------
for name, wf in workflows.items():
    on = wf["on"] if isinstance(wf["on"], dict) else {k: None for k in ([wf["on"]] if isinstance(wf["on"], str) else wf["on"])}
    check("pull_request_target" not in on, f"{name}: no pull_request_target trigger")
    for job_name, job in (wf.get("jobs") or {}).items():
        runs_on = job.get("runs-on", "")
        labels = " ".join(runs_on) if isinstance(runs_on, list) else str(runs_on)
        matrix = str((job.get("strategy") or {}).get("matrix", ""))
        blob = (labels + " " + matrix).lower()
        check("macos" not in blob and "windows" not in blob, f"{name}/{job_name}: no macOS or Windows runner")
        uses = " ".join(str(s.get("uses", "")) for s in job.get("steps", []))
        check("signpath" not in uses.lower(), f"{name}/{job_name}: no SignPath signing step")

ci = workflows["push-pull.yml"]
jobs = ci["jobs"]
check(not {"macOS", "windows", "sign-windows"} & set(jobs), "push-pull.yml: macOS, Windows and signing jobs are gone")

# --- dependencies ---------------------------------------------------------------------
for name, wf in workflows.items():
    names = set(wf.get("jobs") or {})
    for job_name, job in (wf.get("jobs") or {}).items():
        needs = job.get("needs", [])
        needs = [needs] if isinstance(needs, str) else needs
        missing = [n for n in needs if n not in names]
        check(not missing, f"{name}/{job_name}: every needs entry exists {missing or ''}")

publish_needs = set(jobs["publish"]["needs"])
check({"RepositoryPolicy", "CaptureTests", "Linux", "analyze"} <= publish_needs,
      f"release gate needs policy, sanitizer suites, CodeQL and Debian packages ({sorted(publish_needs)})")
check(str(jobs["publish"].get("if", "")).startswith("startsWith(github.event.ref, 'refs/tags')"), "publish runs only for tags")

# --- required suites stay in routine CI -------------------------------------------------
cap = jobs["CaptureTests"]
arches = {m["arch"] for m in cap["strategy"]["matrix"]["include"]}
check(arches == {"x86_64", "arm64"}, f"sanitizer suites run on x86_64 and arm64 ({sorted(arches)})")
steps = " ".join((s.get("name", "") + " " + str(s.get("run", ""))) for s in cap["steps"])
for needle, what in [
    ("tests/v4l2", "V4L2 capture tests"),
    ("tests/bgr24", "BGR24 reference tests"),
    ("tests/color_reference", "colour reference tests"),
    ("tests/codec_test", "decoder regression tests"),
    ("tests/led_output", "LED output and DDP packetization tests"),
    ("tests/cli", "command-line tests"),
    ("tests/diagnostics/doctor-test.sh", "diagnostics checks"),
    ("tests/rk3588/collect-evidence-test.sh", "evidence collector and redaction"),
    ("-fsanitize=address,undefined", "ASan/UBSan"),
]:
    check(needle in steps, f"CaptureTests keeps {what}")
check(jobs["analyze"]["strategy"]["matrix"]["language"] == ["cpp", "javascript"], "CodeQL keeps cpp and javascript")
check("RepositoryPolicy" in jobs and "needs" not in jobs.get("RepositoryPolicy", {}), "repository policy job runs on every event (no change gate)")
for job_name in ("CaptureTests", "Linux", "analyze"):
    check("needs.changes.outputs.code == 'true'" in str(jobs.get(job_name, {}).get("if", "")), f"{job_name} is skipped only for documentation-only changes")

# --- package matrix ---------------------------------------------------------------------
spec = importlib.util.spec_from_file_location("cc", ROOT / "scripts/ci/classify-changes.py")
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)
release = cc.linux_matrix("push", "refs/tags/x", False)
check({"distroName": "debian", "distroVersion": "trixie", "architecture": "arm64"} in release,
      "release validation builds the Debian 13 arm64 package")
check(cc.linux_matrix("push", "refs/heads/x", False) == cc.ROUTINE and len(cc.ROUTINE) == 1,
      "routine pushes build one package (Debian 13 arm64), not the full distribution matrix")
lin = jobs.get("Linux", {"strategy": {"matrix": {}}, "steps": []})
check(lin["strategy"]["matrix"].get("include") == "${{ fromJSON(needs.changes.outputs.linux_matrix) }}",
      "the Linux matrix comes from the change classifier")
smoke = [s for s in lin["steps"] if s.get("name") == "Install package and run command-line checks"]
check(bool(smoke) and "trixie" in str(smoke[0].get("if", "")), "the Debian 13 install and CLI smoke test stays")

# --- triggers, cancellation, permissions ---------------------------------------------
conc = ci.get("concurrency", {})
cancel = str(conc.get("cancel-in-progress", ""))
check("refs/tags/" in cancel and "refs/heads/master" in cancel, "superseded runs cancel, but never tags or master")
check(ci.get("permissions") == {"contents": "read"}, "workflow token defaults to contents: read")
check("workflow_dispatch" in ci["on"] and "compat_matrix" in ci["on"]["workflow_dispatch"]["inputs"],
      "optional compatibility matrix is available by manual dispatch")

# --- no automatic multi-distribution matrix, no ungated upstream publishing ----------
UPSTREAM_GATE = "github.repository == 'awawa-dev/HyperHDR'"
AUTO = {"push", "pull_request", "schedule", "release", "workflow_run"}
for name, wf in workflows.items():
    on = wf["on"] if isinstance(wf["on"], dict) else {k: None for k in ([wf["on"]] if isinstance(wf["on"], str) else wf["on"])}
    automatic = bool(AUTO & set(on))
    text = (WF_DIR / name).read_text()
    for job_name, job in (wf.get("jobs") or {}).items():
        gated = UPSTREAM_GATE in str(job.get("if", ""))
        matrix = (job.get("strategy") or {}).get("matrix", {})
        static = isinstance(matrix, dict) and not str(matrix.get("include", "")).startswith("${{")
        distros = set()
        if static:
            for k in ("distroVersion", "distroName"):
                v = matrix.get(k)
                if isinstance(v, list):
                    distros |= set(map(str, v))
            for inc in matrix.get("include", []) or []:
                if isinstance(inc, dict):
                    distros |= {str(inc.get("distroVersion", "")), str(inc.get("distroName", ""))} - {""}
        check(not (automatic and not gated and len(distros) > 2),
              f"{name}/{job_name}: no automatic multi-distribution matrix ({sorted(distros)[:6]})")
        job_text = str(job)
        # Upstream deploy targets, not mere use of upstream build images or downloads.
        publishes_upstream = "awawa-dev.github.io" in job_text or "'repository-name': 'awawa-dev/" in job_text
        if automatic and publishes_upstream:
            check(gated, f"{name}/{job_name}: upstream publishing is gated to awawa-dev/HyperHDR")
for name in ("image-builder.yml", "image-builder-from-repo.yml"):
    if name in workflows:
        on = workflows[name]["on"]
        check(list(on) == ["workflow_dispatch"], f"{name}: upstream image builder stays manual only")
if "publish-daily-development-build.yml" in workflows:
    for job_name, job in workflows["publish-daily-development-build.yml"]["jobs"].items():
        check(UPSTREAM_GATE in str(job.get("if", "")), f"publish-daily/{job_name}: gated to upstream")
if "upload-to-github-pages.yml" in workflows:
    for job_name, job in workflows["upload-to-github-pages.yml"]["jobs"].items():
        check(UPSTREAM_GATE in str(job.get("if", "")), f"upload-to-github-pages/{job_name}: gated to upstream")

# --- the upstream-sync safeguard itself ------------------------------------------------
guard = workflows.get("fork-guard.yml")
check(guard is not None, "fork-guard.yml exists (fork-only file, survives upstream syncs)")
if guard:
    check({"push", "pull_request"} <= set(guard["on"]), "fork guard runs on push and pull_request")
    check("paths" not in str(guard["on"]) and "paths-ignore" not in str(guard["on"]), "fork guard is never path-filtered")
    g_steps = " ".join(str(st.get("run", "")) for j in guard["jobs"].values() for st in j.get("steps", []))
    check("tests/ci/ci-platform-scope-test.py" in g_steps and "tests/ci/classify-changes-test.py" in g_steps,
          "fork guard runs the CI-scope and change-selection tests")
    check(all("needs" not in j for j in guard["jobs"].values()), "fork guard depends on no other job")
check({"changes", "RepositoryPolicy"} <= set(jobs), "push-pull.yml keeps the change-selection and policy jobs")
rp = " ".join(str(st.get("run", "")) for st in jobs.get("RepositoryPolicy", {}).get("steps", []))
check("tests/ci/ci-platform-scope-test.py" in rp, "the release-gating policy job also runs the CI-scope test")

# --- durable scope documentation ----------------------------------------------------
fork_doc = (ROOT / "docs/fork-scope.md")
check(fork_doc.is_file(), "docs/fork-scope.md exists")
if fork_doc.is_file():
    t = fork_doc.read_text()
    for needle in ("Orange Pi 5 Plus", "Debian 13 arm64", "x86-64", "outside", "manual request", "remain intact", "mandatory"):
        check(needle in t, f"fork-scope.md states the objective containing {needle!r}")
readme = (ROOT / "README.md").read_text()
check("## This fork: Hans5849/HyperHDR" in readme and "docs/fork-scope.md" in readme, "README.md has the fork scope section")

# --- the hardware workflow is not touched by this scope ---------------------------------
board = workflows["rk3588-board.yml"]
check(list(board["on"]) == ["workflow_dispatch"], "rk3588-board.yml stays manual dispatch only")

sys.exit(1 if failures else 0)
