#!/usr/bin/env python3
"""Tests for scripts/ci/classify-changes.py.

Checks the decision table, the fail-safe defaults, the Linux matrices, and the
dependency rule: no tracked file classified docs-only may be read by a build,
package, test or workflow. A path that is only *mentioned* (in a comment or a
help message) is listed in MENTIONED_ONLY with the reason.
"""
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cc", ROOT / "scripts/ci/classify-changes.py")
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)
failures = 0


def check(cond, msg):
    global failures
    print(("PASS " if cond else "FAIL ") + msg)
    failures += 0 if cond else 1


# --- decision table -----------------------------------------------------------------
check(cc.needs_code(["sources/base/Grabber.cpp"]), "source change needs code CI")
check(not cc.needs_code(["docs/rk3588-acceptance-run.md"]), "plain documentation does not")
check(not cc.needs_code(["docs/AGENT_POLICY.md", "AGENTS.md", "CLAUDE.md", "docs/test-registry.json"]),
      "policy documents do not (RepositoryPolicy always runs)")
check(cc.needs_code(["README.md"]), "README.md is the package description: code")
check(cc.needs_code(["docs/rk3588-benchmark-reporting.md"]), "benchmark spec doc is read by bench-validate-test: code")
check(cc.needs_code(["docs/color-patterns/solid-black-1920x1080.png"]), "colour pattern samples are test inputs: code")
check(cc.needs_code(["docs/x.md", "CMakeLists.txt"]), "a mixed change needs code CI")
check(cc.needs_code([".github/workflows/push-pull.yml"]), "workflow change needs code CI")
check(cc.needs_code(["tests/bgr24/README.md"]), "anything under tests/ is code")
check(cc.needs_code([]) and cc.needs_code([""]), "empty diff fails safe to code")
check(cc.needs_code(["docs/sub/dir/file.md"]), "nested docs paths are not assumed docs-only")

# --- matrices -------------------------------------------------------------------------
routine = cc.linux_matrix("push", "refs/heads/feature", False)
check([(m["distroVersion"], m["architecture"]) for m in routine] == [("trixie", "arm64"), ("trixie", "amd64")],
      "routine package builds: Debian 13 arm64 (primary) and amd64 (x86-64 smoke)")
release = cc.linux_matrix("push", "refs/tags/21.0.0.0", False)
check({(m["distroVersion"], m["architecture"]) for m in release} == {("trixie", "arm64"), ("trixie", "amd64")},
      "release: Debian 13 arm64 and amd64")
compat = cc.linux_matrix("workflow_dispatch", "refs/heads/master", True)
keys = [(m["distroName"], m["distroVersion"], m["architecture"]) for m in compat]
check(len(keys) == len(set(keys)), "compatibility matrix has no duplicate entries")
check(len(compat) == 2 + 14, "manual compatibility run: the Debian 13 pair plus 14 optional builds")
check(cc.linux_matrix("pull_request", "refs/pull/1/merge", True) == routine, "compat input ignored outside workflow_dispatch")

# --- end to end through git and the CLI ---------------------------------------------
def run_cli(env, args=()):
    e = dict(os.environ, **env)
    r = subprocess.run([sys.executable, str(ROOT / "scripts/ci/classify-changes.py"), *args],
                       capture_output=True, text=True, env=e, cwd=env.get("_CWD", ROOT))
    return r.returncode, dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l and not l.startswith("changed"))

with tempfile.TemporaryDirectory() as d:
    g = lambda *a: subprocess.run(["git", "-C", d, *a], check=True, capture_output=True, text=True).stdout.strip()
    g("init", "-q", "-b", "master"); g("config", "user.email", "t@t"); g("config", "user.name", "t")
    pathlib.Path(d, "docs").mkdir(); pathlib.Path(d, "docs/a.md").write_text("a\n"); pathlib.Path(d, "x.cpp").write_text("x\n")
    g("add", "-A"); g("commit", "-qm", "base"); base = g("rev-parse", "HEAD")
    pathlib.Path(d, "docs/a.md").write_text("b\n"); g("commit", "-qam", "docs"); docs = g("rev-parse", "HEAD")
    pathlib.Path(d, "x.cpp").write_text("y\n"); g("commit", "-qam", "code"); code = g("rev-parse", "HEAD")
    g("update-ref", "refs/remotes/origin/master", base)        # master is at "base"
    rc, o = run_cli({"EVENT": "push", "REF": "refs/heads/f", "BEFORE": base, "AFTER": docs, "_CWD": d})
    check(rc == 0 and o.get("code") == "false", "branch whose whole change is docs-only: code=false")
    rc, o = run_cli({"EVENT": "push", "REF": "refs/heads/f", "BEFORE": docs, "AFTER": code, "_CWD": d})
    check(o.get("code") == "true", "push of a source commit: code=true")
    # Regression for review RR-001 B1: code commit, then a docs-only commit pushed
    # separately (the code run may have been cancelled). The new head is untested.
    pathlib.Path(d, "docs/a.md").write_text("c\n"); g("commit", "-qam", "docs again"); docs2 = g("rev-parse", "HEAD")
    rc, o = run_cli({"EVENT": "push", "REF": "refs/heads/f", "BEFORE": code, "AFTER": docs2, "_CWD": d})
    check(o.get("code") == "true", "docs-only push on top of an untested code push still runs the code tests")
    rc, o = run_cli({"EVENT": "push", "REF": "refs/heads/f", "BEFORE": "deadbeef" * 5, "AFTER": code, "_CWD": d})
    check(o.get("code") == "true", "unknown 'before' commit (force push): code=true")
    rc, o = run_cli({"EVENT": "push", "REF": "refs/heads/master", "BEFORE": base, "AFTER": docs, "_CWD": d})
    check(o.get("code") == "true", "master always runs everything, even for a docs-only push")
    g("update-ref", "-d", "refs/remotes/origin/master")
    rc, o = run_cli({"EVENT": "push", "REF": "refs/heads/f", "BEFORE": base, "AFTER": docs, "_CWD": d})
    check(o.get("code") == "true", "no origin/master to compare with: fail safe code=true")
    rc, o = run_cli({"EVENT": "push", "REF": "refs/tags/1.0", "BEFORE": docs, "AFTER": code, "_CWD": d})
    check(o.get("code") == "true" and len(json.loads(o["linux_matrix"])) == 2, "tag: everything, release matrix")
    rc, o = run_cli({"EVENT": "workflow_dispatch", "REF": "refs/heads/master", "COMPAT": "true", "_CWD": d})
    check(o.get("code") == "true" and len(json.loads(o["linux_matrix"])) == 16, "manual run with compat: full optional matrix")

# --- dependency rule against the real repository ------------------------------------
MENTIONED_ONLY = {
    "docs/rk3588-board-runner.md": "named in a board-preflight error message",
    "docs/rk3588-latency-measurement.md": "named in optical-latency.py help text",
    "docs/led-output-stats.md": "named in stats-align.py help text",
    "docs/rk3588-frame-stages.md": "named in stats-align.py help and output text",
    "docs/rk3588-color-patterns.md": "named in color-patterns.py help text",
    "BUILDING.md": "named in a hyperhdr-doctor.sh warning message",
    "docs/diagnostics.md": "named in a hyperhdr-doctor.sh comment",
    "docs/rk3588-board-session.md": "named in an rk3588-board.yml comment",
    # Read only by tests/ci/agent-entrypoints-test.py, which runs in the always-on
    # RepositoryPolicy job; the policy branch also names them in a step title.
    "docs/fork-scope.md": "named in workflow comments; read only by the always-on CI-scope test",
    "AGENTS.md": "read only by the always-on repository-policy tests",
    "CLAUDE.md": "read only by the always-on repository-policy tests",
}
tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True, check=True).stdout.split()
docs_only = [p for p in tracked if cc.is_docs_only(p)]
consumers = [p for p in tracked if (re.match(r"(tests|scripts|cmake|sources|\.github/(workflows|actions))/", p)
             or p.endswith("CMakeLists.txt")) and not p.endswith(".md")]   # a markdown link is not a read
text = {}
for p in consumers:
    try:
        text[p] = (ROOT / p).read_text(errors="ignore")
    except (IsADirectoryError, FileNotFoundError):
        pass
policy_readers = {"tests/ci/agent-entrypoints-test.py", "tests/ci/classify-changes-test.py", "scripts/ci/classify-changes.py",
                  "tests/ci/ci-platform-scope-test.py"}
for p in docs_only:
    users = [c for c, t in text.items() if p in t and c not in policy_readers]
    if users and p not in MENTIONED_ONLY:
        check(False, "%s is classified docs-only but referenced by %s" % (p, ", ".join(users)))
check(any(p == "docs/rk3588-acceptance-run.md" for p in docs_only), "real docs files are recognised as docs-only")
check(all(not cc.is_docs_only(p) for p in tracked if p.startswith("docs/color-patterns/")), "no colour-pattern sample is docs-only")

sys.exit(1 if failures else 0)
