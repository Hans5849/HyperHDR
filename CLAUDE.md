# Claude Code: HyperHDR release sprint

Repository: `Hans5849/HyperHDR`. Default branch: `master`.
Updated: 2026-11-19. This file is an operating brief, not a claim that physical hardware validation has passed.

## Mission and release boundary

Deliver the smallest stable and measurable **Orange Pi 5 Plus (RK3588) native HDMI input -> HyperHDR CPU processing -> DDP -> ESP32/WLED LEDs** release. Optimize for **verified end-to-end working behavior**, not the number of commits or issues closed. Preserve every proven color, latency, reliability and security property.

First release **does not depend on** GPU, RGA, NPU, DMA-BUF zero-copy, or general vector-decoder performance optimization. Issues #13-#17 are deferred unless an actual measured defect blocks the CPU release. The RK3588 hardware has not yet passed the end-to-end application acceptance procedure. Do not infer it has from hosted ARM64 tests, kernel-level capture, or a successful package build.

## Start every scheduled or interactive session

1. `git fetch origin`; inspect actual `master` HEAD, working tree, open PRs, open issues, review threads, branch protections, CI and any recent merges. Never assume this document's issue status remains current.
2. Read `docs/rk3588-acceptance-run.md`, `docs/rk3588-hardware-validation.md`, `docs/rk3588-operator-checklist.md`, `docs/rk3588-board-session.md`, and `docs/rk3588-development-results.md`. Treat operator observations and unverified hypotheses differently.
3. Review blockers first: #8 (actual HDMI -> HyperHDR -> WLED), #10 (image fidelity), #9 (benchmark), #11/#19 (recovery), #27 (diagnostics), #28 (dedicated runner). Issue #7 is the parent roadmap. Review #61/#64/#68/#71 for distinct shared decoder defects. The P010 crop fix #69 merged and #65 was superseded; never recreate it.
4. Prioritize the highest-impact next testable task, select a **single well-bounded change per branch**, and avoid duplicating active PR work. If other sessions are working concurrently, claim the issue in its thread and use isolated branches/worktrees.
5. Before editing, write a concrete hypothesis, expected failure, test, measurement method, and rollback. For bugs, add a failing regression test first when feasible.
6. Before PR submission, run targeted tests, required broader suites, applicable sanitizers, style/security checks, and document platform limits. Record exact HEAD SHA, command lines and results. Mark unavailable tests as unverified, never passed.
7. Open a focused PR linked to issue(s), describing change, before/after evidence, risk and rollback. Reconcile every unresolved review thread and every red CI result before proposing merge.
8. At end of each run, leave a short issue/PR status update: HEAD, what changed, CI and tests, next action, blocker, whether operator intervention is needed. Do not write recurring duplicate comments when there was no change.

## Strict safety and autonomy policy

- **Never** auto-deploy, upgrade kernel, flash boot media, power-cycle, reboot, stop the live HyperHDR service, change source/TV/WLED settings, or claim physical video/LED behavior without the operator's approval and evidence.
- Hardware-in-loop workflows use the actual uniquely labeled Orange Pi runner and manually approved `rk3588-hil` environment. Never route arbitrary pull-request code onto the production board. Only trusted, reviewed `master` code and explicit operator approval may use board workflows. Do not loosen protections to speed things up.
- No secrets, credentials, tokens, private network identifiers, or captured sensitive HDMI content in commits, comments, artifacts, logs or public PRs. Use minimal permissions, least privilege, pinned CI action versions, dependency reviews and secret scans.
- Do not force push another session's branch. Never close issues just because a PR was created or hosted CI passed; close only when the issue's actual acceptance criteria are met, with a reference to evidence.
- Automatic merge is allowed only for **low-risk, independently verified, non-hardware** changes when all applicable CI checks pass, no reviews are outstanding, no conflicts exist, the head SHA matches the reviewed revision and repository rules permit it. Never auto-merge an unresolved security-performance tradeoff, behavioral video change, dependency major version, or hardware/production integration change.
- Treat hosted CI runner capacity failures separately from code failures: inspect steps and logs; retry only capacity cancellations when justified, never silently waive a real failure.
- Tests may run concurrently, but protect CPU, memory, disk and CI budget. Avoid multiple full multi-platform matrices for overlapping draft revisions.

## Work queues and order

**Lane A - release critical (always first):**
- Run the release-candidate checklist on a pinned `master` commit, starting with a verified, restorable installed `.deb` and settings backup. The operator must perform steps involving physical source, TV, LEDs, exclusive receiver access or service interruption.
- Confirm native receiver identity by driver/bus, not a hardcoded `/dev/videoN`; verify live BGR24 preview, 1080p and supported 4K timings; orientation, RGB order and full/limited range; DDP receipt and WLED LED order/color; independent capture and output rates; no-source/reconnect; 30-minute stability. Attach evidence to #8/#9/#10/#11/#19/#27/#28.
- Inspect any failures, reproduce with a minimal host-side or board-safe test, fix only demonstrated defects, and rerun the exact failed acceptance step. Do not call the release ready until *every required acceptance step* has evidence.

**Lane B - safety and CI (parallel, but may not distract Lane A):**
- PR #64 fixes real vector decoder bounds/UB issues (#61), but conflicts with merged #69 and has synthetic throughput cost recorded in #71. Rebase onto current master; reconcile P010 crop independently; keep ASan/UBSan, exact-size buffers, guard-page and sentinel tests; run full suite. No merge until the tradeoff is documented and safe, reviewed, and CI green.
- Issue #68 covers additional unaligned/over-long reads in tone-mapping scan and LUT checksum; address in separate targeted PRs with regression tests.
- Keep CI portability and build reproducibility intact; avoid runner explosion or redundant tests. Preserve security scanners.

**Lane C - after release:**
- Only pursue #12-#17 acceleration/zero-copy/NEON with on-board evidence and an explicit benchmark including quality, end-to-end latency, CPU/RAM/temperature and fallback. No hardware support claims without driver validation.

## Release acceptance and stop condition

Stop feature expansion when the acceptance checklist is all green on the **same pinned candidate SHA**, with restored rollback proven and evidence links attached. Publish a concise release-readiness report: exact package SHA, OS/kernel/build/toolchain, physical test environment, per-step PASS/FAIL/NOT RUN, regressions, unresolved risks and whether shipping is recommended. Do not treat the absence of issues as proof of success.

## Recommended automation separation

Run **one primary cloud scheduled task every 4 hours** for backlog triage, implementing one isolated release-critical issue, reviewing CI and opening/updating PRs. Configure a **GitHub PR-event review task** for focused test/review follow-ups if supported; avoid overlapping writes to the same branch. Use a separate **daily read-only release-readiness report** only if run quotas permit; otherwise include that report in the primary task's final comment. Cloud schedules are configured in Claude Code (/schedule or Claude Code Scheduled tasks), **not** by this file or by a GitHub Actions cron alone. Do not assume a task exists until Claude confirms its creation.
