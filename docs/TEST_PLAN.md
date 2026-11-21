# Test plan

Policy revision: `AP-2026.11.21-6` · Registry: [test-registry.json](test-registry.json) (one entry per required test: ID, requirement, purpose, prerequisites, exact command, environment, expected result, gate, owner, cost). Security-relevant tests carry `security_ids` that map to [SECURITY_PLAN.md](SECURITY_PLAN.md) §3. Update the registry with every test change; `T-AGENT-ENTRYPOINTS` checks that every registry ID is listed below.

## Gates

| Gate | Tests | Where |
|---|---|---|
| **Merge** (every PR, exact head SHA) | T-V4L2, T-BGR24, T-COLOR-REF, T-DECODER, T-BENCH-REPORT, T-LED-OUTPUT, T-CLI, T-COLOR-PATTERNS, T-OPTICAL-LATENCY, T-DEB-VERSION, T-BOARD-ARTIFACT, T-STATS-ALIGN, T-EVIDENCE, T-DOCTOR, T-BOARD-POLICY, T-AGENT-ENTRYPOINTS, T-CI-SCOPE, T-CI-CLASSIFY, T-PKG-LINUX (Debian 13 arm64) | Hosted CI (`.github/workflows/push-pull.yml`), x86_64 and arm64, ASan/UBSan. Documentation-only changes run only the repository-policy tests (T-AGENT-ENTRYPOINTS, T-CI-SCOPE, T-CI-CLASSIFY, T-BOARD-POLICY); see `docs/ci-scope.md` |
| **Optional** (manual dispatch) | T-PKG-COMPAT | Hosted CI, `compat_matrix=true` |
| **Release** (one pinned candidate SHA) | All merge-gate tests on the final `master` commit, plus T-BOARD-HIL and T-ACCEPT-RK3588 | Hosted CI, the Orange Pi 5 Plus via `rk3588-hil`, and the operator |

**Platform scope.** Linux only (owner, 2026-11-21). Debian 13 arm64 is the primary package; x86_64 Linux is for development and testing. Windows and macOS are outside this fork's supported scope: they are not built or tested, and no result may be read as evidence that they work.

Pass/fail: a test passes only when it exits 0 on the exact SHA under evaluation. Pending, cancelled, skipped-required or old-SHA results are not passes. Hosted ARM64 results are never hardware acceptance.

## Running locally

Install `cmake g++ qt6-base-dev libturbojpeg0-dev python3 python3-yaml`, then run the commands in the registry from a clean worktree at the repository root. Development may use targeted tests; merge and release use the full gate.

## Results and retrieval

**Result records.** Each result record identifies:
- the full candidate SHA or artifact digest;
- the environment and tool versions;
- start and end times, and the exit status;
- the outcome of each shard;
- log and artifact links;
- limitations, and retention and access.

A missing shard, architecture or required check leaves the gate pending. An environment failure is kept distinct from an implementation failure, and neither counts as a pass.

- CI results: the workflow run for the head SHA (`Actions → HyperHDR CI Build`), job logs and uploaded artifacts. Retention follows the repository's Actions retention setting (see RESOURCE_LIMITS.md).
- Each PR gate table and cycle report records SHA, date, commands, per-test outcome, run links and limitations.
- Board and acceptance evidence: artifacts from `rk3588-board.yml` and the records defined in `docs/rk3588-acceptance-run.md`, attached to the relevant issue.
