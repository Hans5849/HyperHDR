# CI scope for this fork (Linux only)

**Owner decision (2026-11-21):** Windows and macOS are not supported development targets of `Hans5849/HyperHDR`, so their routine and release CI jobs were removed. This fork **makes no claim** that HyperHDR builds or works on Windows or macOS. Their application source and build-system support (`build.sh`, `cmake/installer_windows.cmake`, `cmake/installer_macos.cmake`, `BUILDING.md`) are kept unchanged, so another platform can be reinstated later.

**Primary target:** Orange Pi 5 Plus (RK3588), Debian 13 arm64 (Armbian).

## What runs, and when (`.github/workflows/push-pull.yml`)

| Job | Push or PR changing code | Documentation-only change | Tag (release) | Manual run |
|---|---|---|---|---|
| `changes`: select CI work (`scripts/ci/classify-changes.py`) | yes | yes | yes | yes |
| `RepositoryPolicy`: CI-scope, board-workflow, cache-cleaner and agent-policy tests | yes | yes | yes | yes |
| `CaptureTests`: sanitizer suites (V4L2, BGR24, colour, decoder, benchmark reporting, LED/DDP, CLI, diagnostics, evidence, packaging metadata, board preflight), x86_64 **and** arm64 | yes | skipped | yes | yes |
| `analyze`: CodeQL for cpp and javascript | yes | skipped | yes | yes |
| `Linux`: Debian 13 **arm64** package, `.deb` metadata check, and install plus CLI smoke test | yes | skipped | yes | yes |
| `Linux`: Debian 13 **amd64** package | — | — | yes | yes |
| `Linux`: optional compatibility packages (Debian 11–14 amd64/armhf/arm64, Ubuntu, Fedora, Arch) | — | — | — | only with `compat_matrix` |
| `publish`: draft GitHub release | — | — | needs `RepositoryPolicy`, `CaptureTests`, `Linux` and `analyze` | — |

**Documentation-only** means every changed path is documentation that no build, package, test or workflow reads. The classifier decides this from dependencies, not from file extensions:
- `README.md` is the package description, so it counts as code.
- `docs/color-patterns/**` and `docs/rk3588-benchmark-reporting.md` are test inputs, so they count as code.
- `tests/ci/classify-changes-test.py` checks every documentation file against the repository's build, test and workflow files.
- Unknown cases, tags, manual runs and force pushes fail safe to running everything.

**Duplicates and cancellation.**
- A same-repository PR doesn't rerun CI; the push run covers its head. Fork PRs run on `pull_request`, without secrets.
- A newer push to the same branch or PR cancels the superseded run.
- Tag runs (release validation) and `master` runs are **never** cancelled.

**Hardware.** `rk3588-board.yml`, the self-hosted Orange Pi runner, is unchanged. It stays manual dispatch only, from `master`, through the `rk3588-hil` environment.

## Running the optional compatibility builds

Actions → *HyperHDR CI Build* → **Run workflow**, then tick `compat_matrix`. This builds the 16 packages: the Debian 13 pair plus the 14 optional builds.

## Reinstating Windows or macOS

The removed jobs (`macOS`, `windows` and `sign-windows`, the last using SignPath with the repository secrets `SIGNPATH_API_TOKEN` and `SIGNPATH_ORGANIZATION_ID`) are in `.github/workflows/push-pull.yml` at commit `f091f7c3d4a210bc424a7d2ae31c8559f754b2e1`.

To reinstate one:
1. Copy the job back.
2. Add it to `publish.needs` if it should gate releases.
3. Adjust `tests/ci/ci-platform-scope-test.py`, which currently fails on any Windows or macOS runner.

That is a CI-workflow change (RED tier). It needs owner authorization and F review.
