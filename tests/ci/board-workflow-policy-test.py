#!/usr/bin/env python3
"""Trust-boundary checks for .github/workflows/rk3588-board.yml (issue #28).

The board runner is a real device in someone's home. These invariants keep
untrusted code and accidental runs off it:
- manual dispatch is the only trigger (no pull_request, push or schedule)
- master only, through the rk3588-hil environment (reviewers can gate runs)
- the job needs the Orange Pi-specific labels, not just "rk3588"
- read-only token, preflight first, no sudo, no service control
"""
import pathlib, re, sys
import yaml

path = pathlib.Path(__file__).resolve().parents[2] / ".github/workflows/rk3588-board.yml"
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
    check("github.ref == 'refs/heads/master'" in cond and "github.repository == 'Hans5849/HyperHDR'" in cond,
          f"{name}: master of this repository only ({cond})")
    check(job.get("environment") == "rk3588-hil", f"{name}: runs in the rk3588-hil environment")
    steps = job.get("steps", [])
    runs = [s for s in steps if "run" in s]
    check(bool(runs) and "board-preflight.sh" in runs[0]["run"], f"{name}: preflight is the first command step")
    script = "\n".join(s["run"] for s in runs)
    check(not re.search(r"(^|[\s;&|(])sudo\s", script), f"{name}: no sudo")
    check(not re.search(r"systemctl\s+(stop|start|restart|disable|enable|kill)|\bpkill\b|\bkill\s", script),
          f"{name}: never controls services or processes")
    check(not re.search(r"(^|\s)(apt|apt-get|dpkg|usermod|chown)\s", script), f"{name}: never changes packages or users")
    uses = [s["uses"] for s in steps if "uses" in s]
    check(all(re.match(r"actions/(checkout|upload-artifact)@v\d", u) for u in uses), f"{name}: only first-party actions ({uses})")

print(f"{len(failures)} failure(s)" if failures else "all board-workflow policy checks passed")
sys.exit(1 if failures else 0)
