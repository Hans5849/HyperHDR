#!/usr/bin/env python3
"""Operating-standard check: AGENTS.md and CLAUDE.md exist, link each other and the
shared policy sources, carry the same policy revision as those sources, and every
test-registry ID is listed in docs/TEST_PLAN.md.

  agent-entrypoints-test.py [REPO_ROOT]
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2])
failures = 0


def check(cond, msg):
    global failures
    print(("PASS " if cond else "FAIL ") + msg)
    failures += 0 if cond else 1


def revision(path):
    m = re.search(r"Policy revision: `([^`]+)`", path.read_text(encoding="utf-8"))
    return m.group(1) if m else None


entry = {name: ROOT / name for name in ("AGENTS.md", "CLAUDE.md")}
shared = ["docs/AGENT_POLICY.md", "docs/AGENT_WORKFLOW.md", "docs/AGENT_ADAPTER.md", "docs/PROJECT_PLAN.md",
          "docs/TEST_PLAN.md", "docs/test-registry.json", "docs/RESOURCE_LIMITS.md",
          "docs/INSTALLATION_PROFILE.md", "docs/SECURITY_PLAN.md"]

for name, path in entry.items():
    check(path.is_file(), f"{name} exists")
for rel in shared:
    check((ROOT / rel).is_file(), f"{rel} exists")
if failures:
    sys.exit(1)

texts = {n: p.read_text(encoding="utf-8") for n, p in entry.items()}
check("(CLAUDE.md)" in texts["AGENTS.md"], "AGENTS.md links CLAUDE.md")
check("(AGENTS.md)" in texts["CLAUDE.md"], "CLAUDE.md links AGENTS.md")
for name, text in texts.items():
    for rel in shared:
        check(f"({rel})" in text, f"{name} references {rel}")
    pm = re.findall(r"PM Control \(canonical dashboard\): issue #(\d+)", text)
    check(len(set(pm)) == 1, f"{name} names exactly one PM Control issue")
    check("Operations log" in text, f"{name} records the operations log (sprint coordination) issue")

revs = {n: revision(p) for n, p in entry.items()}
for rel in ("docs/AGENT_POLICY.md", "docs/AGENT_WORKFLOW.md", "docs/AGENT_ADAPTER.md", "docs/PROJECT_PLAN.md",
            "docs/TEST_PLAN.md", "docs/RESOURCE_LIMITS.md",
            "docs/INSTALLATION_PROFILE.md", "docs/SECURITY_PLAN.md"):
    revs[rel] = revision(ROOT / rel)
revs["docs/test-registry.json"] = json.loads((ROOT / "docs/test-registry.json").read_text())["policy_revision"]
check(None not in revs.values(), "every policy file states a revision")
check(len(set(revs.values())) == 1, "one policy revision everywhere: %s" % sorted(set(map(str, revs.values()))))
pms = {m for t in texts.values() for m in re.findall(r"PM Control \(canonical dashboard\): issue #(\d+)", t)}
check(len(pms) == 1, "both entrypoints name the same PM Control issue")

plan = (ROOT / "docs/TEST_PLAN.md").read_text(encoding="utf-8")
ids = [t["id"] for t in json.loads((ROOT / "docs/test-registry.json").read_text())["tests"]]
check(len(ids) == len(set(ids)), "test-registry IDs are unique")
missing = [i for i in ids if i not in plan]
check(not missing, "every registry ID appears in TEST_PLAN.md %s" % (missing or ""))
sec = (ROOT / "docs/SECURITY_PLAN.md").read_text(encoding="utf-8")
sids = sorted({s for t in json.loads((ROOT / "docs/test-registry.json").read_text())["tests"] for s in t.get("security_ids", [])})
check(bool(sids), "test registry links security test IDs")
check(all(s in sec for s in sids), "every registry security ID is defined in SECURITY_PLAN.md")

sys.exit(1 if failures else 0)
