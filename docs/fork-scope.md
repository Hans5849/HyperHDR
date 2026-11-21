# Fork scope: platforms, CI and release requirements

Applies to `Hans5849/HyperHDR`, a public development fork of [awawa-dev/HyperHDR](https://github.com/awawa-dev/HyperHDR). Owner decision of 2026-11-20 and 2026-11-21, tracked in issue #81. This page describes **this fork only**; it says nothing about upstream's platform support.

## Objectives

1. **Primary target.** The Orange Pi 5 Plus (RK3588) running Armbian Debian 13 arm64 is the primary supported deployment and release target.
2. **x86-64 Linux.** It remains supported for development, automated regression testing and CI.
3. **Windows and macOS.** They are **outside** this fork's supported development, testing and release scope. The fork does not build or test them and makes no claim that they work.
4. **Other Linux distributions.** These are not routine release targets. Compatibility testing for them runs only on manual request.
5. **Code kept for later.** Windows and macOS source code and build-system support (`build.sh`, `cmake/installer_windows.cmake`, `cmake/installer_macos.cmake`, the platform grabbers, `BUILDING.md`) remain intact, so support can be restored.
6. **Testing stays mandatory.** All RK3588-relevant functional, sanitizer, security, regression and release-acceptance testing remains mandatory.

## Automatic CI (`.github/workflows/push-pull.yml`)

| Job | Code change (push / fork PR) | Docs-only change | Tag (release) | Manual run |
|---|---|---|---|---|
| `changes`: selects the work (`scripts/ci/classify-changes.py`) | yes | yes | yes | yes |
| `RepositoryPolicy`: CI-scope, change-selection, board-workflow, cache-cleaner and agent-policy tests | yes | yes | yes | yes |
| `CaptureTests`: x86_64 **and** arm64, ASan/UBSan. Covers V4L2 capture, BGR24, colour reference, decoder regression, benchmark reporting, LED/DDP, CLI, colour patterns, optical latency, Debian version mapping, board artifact, statistics alignment, evidence collector (redaction), diagnostics, board preflight and workflow policy | yes | skipped | yes | yes |
| `analyze`: CodeQL for cpp and javascript | yes | skipped | yes | yes |
| `Linux`: Debian 13 **arm64** `.deb` (primary), metadata check, and install plus CLI smoke test | yes | skipped | yes | yes |
| `Linux`: Debian 13 **amd64** `.deb` (x86-64 build and smoke check), metadata check, and install plus CLI smoke test | yes | skipped | yes | yes |
| `Linux`: optional compatibility packages, 14 of them (Debian 11–14 amd64/armhf/arm64, Ubuntu noble/resolute, Fedora 44, Arch) | — | — | — | only with `compat_matrix` |
| `publish`: draft release | — | — | needs `RepositoryPolicy`, `CaptureTests`, `Linux`, `analyze` | — |

**Change selection** is dependency-based, not extension-based:
- `README.md` is the package description, so it counts as code.
- `docs/color-patterns/**` and `docs/rk3588-benchmark-reporting.md` are test inputs, so they count as code.
- `tests/ci/classify-changes-test.py` audits every documentation file against the build, package, test and workflow files.
- A branch push is classified by the **whole** difference between the branch and `master`, not just its newest commit. So a docs-only commit pushed on top of an untested code commit still runs the code tests.
- `master`, tags, manual runs, a missing `origin/master` and any git error always run everything.

**Duplicates and cancellation.**
- A same-repository PR doesn't rerun CI; checks are reported on the head SHA by the push run.
- Fork PRs run on `pull_request`, without secrets and never on self-hosted runners.
- A newer push cancels a superseded development run.
- Tags and `master` get a unique concurrency group for each run, so **neither a running nor a pending tag or `master` run is ever cancelled or replaced**.

**Caching and retention.**
- ccache is opt-in (`vars.USE_CACHE`, default off). The package jobs delete their branch caches after each run, so a cross-run cache would need a cleanup-policy change first. Evaluated and left unchanged.
- Commit artifacts are kept for 7 days and release artifacts for 14 days.

## Every workflow

| Workflow | Trigger | In this fork |
|---|---|---|
| `push-pull.yml` | push, PR, manual | The table above |
| `fork-guard.yml` (fork-only) | push, PR, manual | Runs the CI-scope and change-selection tests on every change. Never path-skipped. Upstream has no file of this name, so a sync can't remove it |
| `rk3588-board.yml` | manual only | Self-hosted Orange Pi runner; `master` only, through the `rk3588-hil` environment. **Unchanged** |
| `cache-cleaner.yml` | PR closed | Deletes that PR's caches. Small |
| `upload-to-github-pages.yml` | tag push, manual | Upstream publishing to `awawa-dev.github.io`. **Disabled** in this fork's Actions settings, and every job is gated to `awawa-dev/HyperHDR` |
| `publish-daily-development-build.yml` | daily schedule, manual | Upstream development release. The job is gated to `awawa-dev/HyperHDR`, so the daily trigger here creates a skipped run (0 runner-minutes). Disabling it in the Actions UI would also remove that run entry |
| `image-builder.yml`, `image-builder-from-repo.yml` | manual only | Raspberry Pi images from upstream releases. Never automatic; not used by this fork |

## Required status checks

- **Branch protection.** No agent tool can read branch protection or rulesets. The owner must confirm that no removed job is required: `Windows`, `macOS 15 (x64)`, `macOS 15 (arm64: M1/M2/M3/M4)`, `Sign Windows Installer`, and every Linux matrix name except `arm64/debian:trixie`.
- **Recommended required checks:** `Fork CI guard` and `Repository policy checks` (both always run), plus `arm64/debian:trixie` and the two `V4L2 capture and BGR24 decoder tests …` jobs.
- **Fork guard enforcement.** `Fork CI guard` detects a reintroduced platform or matrix, but it only *prevents* one when it is a **required check** on `master` and upstream syncs land **through a PR**, never by a direct push. Both are owner settings: a branch ruleset on `master` that requires a PR and requires `Fork CI guard` (security review RR-002, F82-4).
- **Skipped jobs.** GitHub treats a job skipped by its `if:` condition (for example on a documentation-only change) as passing a required check, so path selection never leaves a required check pending.

## Release requirements

A release candidate needs the `publish` gates above to pass on the tag's exact SHA:
- repository policy;
- sanitizer suites on x86_64 and arm64;
- CodeQL;
- the Debian 13 arm64 and amd64 packages, with the install and CLI smoke test.

It then also needs the hardware acceptance in `docs/rk3588-acceptance-run.md`, on the Orange Pi with the operator. Hosted CI never replaces hardware acceptance.

## Upstream synchronization

When merging from `awawa-dev/HyperHDR`, conflicts in `push-pull.yml` are expected. Resolve them in favour of this fork's jobs.

`tests/ci/ci-platform-scope-test.py` runs from both `push-pull.yml` and the fork-only `fork-guard.yml`, and **fails** if a sync brings back any of these:
- a Windows or macOS runner;
- SignPath signing;
- an automatic multi-distribution matrix;
- a dangling `needs`;
- ungated upstream publishing;
- `pull_request_target`;
- a missing required suite;
- a change to the board workflow's dispatch-only rule.

Upstream syncs are reviewed by D and F like dependency updates (`docs/AGENT_POLICY.md` on the policy branch).

## Reinstating Windows or macOS

The removed jobs (`macOS`, `windows`, and `sign-windows`, which used SignPath with `SIGNPATH_API_TOKEN` and `SIGNPATH_ORGANIZATION_ID`) are in `.github/workflows/push-pull.yml` at commit `f091f7c3d4a210bc424a7d2ae31c8559f754b2e1`.

To reinstate one:
1. Copy the job back.
2. Add it to `publish.needs` if it should gate releases.
3. Update this page and `tests/ci/ci-platform-scope-test.py`.

This is a protected CI change (RED tier). It needs owner authorization and D and F review.
