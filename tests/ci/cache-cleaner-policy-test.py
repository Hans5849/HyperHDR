#!/usr/bin/env python3
"""Least-privilege checks for .github/workflows/cache-cleaner.yml.

The artifact job needs actions: write to delete artifacts (it failed with HTTP 403
"Resource not accessible by integration" without it). It must get that and nothing
more, never run pull-request code, and skip fork PRs (read-only token; branch-name
lookup could match our branches).
"""
import pathlib, re, sys
import yaml

wf = yaml.safe_load((pathlib.Path(__file__).resolve().parents[2] / ".github/workflows/cache-cleaner.yml").read_text())
on = wf.get(True, wf.get("on"))
failures = []
def check(cond, msg):
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond: failures.append(msg)

check(list(on) == ["pull_request"] and on["pull_request"]["types"] == ["closed"], "triggered by pull_request closed only (not pull_request_target)")
check(wf.get("permissions") == {}, "workflow default token permissions are empty")
for name, job in wf["jobs"].items():
    perms = job.get("permissions")
    check(perms == {"actions": "write"}, f"{name}: exactly actions: write ({perms})")
    for step in job.get("steps", []):
        uses = step.get("uses", "")
        check(not uses.startswith("actions/checkout"), f"{name}: checks out nothing")
        if uses and not uses.startswith("actions/"):
            check(re.fullmatch(r"[\w.-]+/[\w.-]+@[0-9a-f]{40}", uses.split()[0]) is not None, f"{name}: third-party action pinned to a commit ({uses})")
art = wf["jobs"]["clean_PR_artifacts"]
check(art.get("if", "").replace(" ", "") == "github.event.pull_request.head.repo.full_name==github.repository", "artifact job skips fork PRs")
check(art["steps"][0]["with"]["workflow_filename"] == "push-pull.yml", "artifact job targets push-pull.yml runs")

print(f"{len(failures)} failure(s)" if failures else "all cache-cleaner policy checks passed")
sys.exit(1 if failures else 0)
