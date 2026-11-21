#!/usr/bin/env python3
"""Decide which CI work a change needs (.github/workflows/push-pull.yml, job "changes").

Outputs, written as key=value lines to --github-output (or stdout):
  code=true|false   false only when every changed path is documentation that
                    no build, package, test or workflow reads (DOCS_ONLY below)
  linux_matrix=JSON the Linux package matrix for this event

The decision is dependency-based, not extension-based:
- README.md is the CPack package description (cmake/packages.cmake), so it is code.
- docs/color-patterns/** and docs/rk3588-benchmark-reporting.md are read by tests
  (tests/ci/color-patterns-test.py, tests/ci/bench-validate-test.py), so they are code.
- Policy documents read only by the repository-policy tests are not "code": the
  RepositoryPolicy job runs on every event regardless of this output.
Anything unknown, any git error, tags, workflow_dispatch and an empty diff all
yield code=true (fail safe: run everything).

  classify-changes.py [--github-output FILE] [--paths-from FILE]
Event context comes from environment variables set by the workflow:
  EVENT, REF, BEFORE, AFTER, COMPAT ("true" adds the optional compatibility matrix)
"""
import re
import json
import os
import subprocess
import sys

# Documentation that nothing builds, packages, tests or runs. Keep this list
# narrow; tests/ci/classify-changes-test.py checks it against the repository.
DOCS_ONLY = [
    "docs/*.md",            # except the files in CONSUMED below
    "AGENTS.md", "CLAUDE.md", "SECURITY.md",
    "BUILDING.md", "CHANGELOG.md", "CODE_OF_CONDUCT.md", "CODE_SIGNING_POLICY.md",
    "docs/test-registry.json",   # read only by the repository-policy tests
    ".github/ISSUE_TEMPLATE/*", ".github/PULL_REQUEST_TEMPLATE.md", ".github/FUNDING.yml",
]
# Files that match DOCS_ONLY patterns but are inputs to a build, package or test.
CONSUMED = [
    "docs/rk3588-benchmark-reporting.md",   # tests/ci/bench-validate-test.py compares it with the spec
    "docs/color-patterns/*",                # tests/ci/color-patterns-test.py
    "README.md",                            # CPACK_PACKAGE_DESCRIPTION_FILE
]

ROUTINE = [{"distroName": "debian", "distroVersion": "trixie", "architecture": "arm64"}]
RELEASE = ROUTINE + [{"distroName": "debian", "distroVersion": "trixie", "architecture": "amd64"}]
COMPAT = [{"distroName": "debian", "distroVersion": v, "architecture": a}
          for v in ("bullseye", "bookworm", "trixie", "forky")
          for a in ("amd64", "armhf", "arm64")
          if not (v == "trixie" and a in ("arm64", "amd64"))] + [
    {"distroName": "ubuntu", "distroVersion": "noble", "architecture": "amd64"},
    {"distroName": "ubuntu", "distroVersion": "resolute", "architecture": "amd64"},
    {"distroName": "fedora", "distroVersion": "44", "architecture": "amd64"},
    {"distroName": "archlinux", "distroVersion": "latest", "architecture": "amd64"},
]


def _match(path, patterns):
    """Glob match where '*' never crosses '/', so docs/*.md excludes docs/sub/x.md."""
    for p in patterns:
        rx = "".join("[^/]*" if c == "*" else re.escape(c) for c in p)
        if re.fullmatch(rx, path):
            return True
    return False


def is_docs_only(path):
    return _match(path, DOCS_ONLY) and not _match(path, CONSUMED)


def needs_code(paths):
    """True unless there is at least one path and every path is docs-only."""
    paths = [p for p in paths if p]
    return not paths or not all(is_docs_only(p) for p in paths)


def linux_matrix(event, ref, compat):
    if ref.startswith("refs/tags/"):
        return RELEASE
    if event == "workflow_dispatch" and compat:
        return RELEASE + COMPAT
    return ROUTINE


def _git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def changed_paths(event, before, after):
    """Changed paths for this event, or None when they can't be determined."""
    try:
        if event == "pull_request":
            # actions/checkout checks out the merge commit; parent 1 is the base.
            return _git("diff", "--name-only", "HEAD^1", "HEAD").split("\n")
        if event == "push":
            if before and set(before) != {"0"}:
                _git("cat-file", "-e", before + "^{commit}")
                return _git("diff", "--name-only", before, after).split("\n")
            base = _git("merge-base", "origin/master", after).strip()   # new branch
            return _git("diff", "--name-only", base, after).split("\n")
    except (subprocess.CalledProcessError, OSError):
        return None
    return None


def main(argv):
    out_file, paths_file = None, None
    i = 0
    while i < len(argv):
        if argv[i] == "--github-output":
            out_file, i = argv[i + 1], i + 2
        elif argv[i] == "--paths-from":
            paths_file, i = argv[i + 1], i + 2
        else:
            print("unknown argument %r" % argv[i], file=sys.stderr)
            return 2
    event = os.environ.get("EVENT", "")
    ref = os.environ.get("REF", "")
    compat = os.environ.get("COMPAT", "") == "true"
    if paths_file:
        paths = open(paths_file).read().split("\n")
    elif ref.startswith("refs/tags/") or event not in ("push", "pull_request"):
        paths = None                      # releases and manual runs: everything
    else:
        paths = changed_paths(event, os.environ.get("BEFORE", ""), os.environ.get("AFTER", "HEAD"))
    code = True if paths is None else needs_code(paths)
    lines = ["code=%s" % ("true" if code else "false"),
             "linux_matrix=%s" % json.dumps(linux_matrix(event, ref, compat), separators=(",", ":"))]
    print("changed paths: %s" % ("unknown (fail safe)" if paths is None else ", ".join(p for p in paths if p) or "none"))
    print("\n".join(lines))
    if out_file:
        with open(out_file, "a") as f:
            f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
