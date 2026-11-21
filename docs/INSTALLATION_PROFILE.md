# Installation profile

Policy revision: `AP-2026.11.21-8` · Profile revision: `IP-3` · Accountable owner: Hans5849 · Last verified: 2026-11-21 (from repository docs and code only; **not** re-observed on the target). `IP-3` changes only P3 (hosted CI, Linux-only scope); P1, P2 and P4 are unchanged from `IP-2`

## Repository security profile (standard page 18)

**Profile: Public fork.** Owner-stated on 2026-11-20. This repository is a public fork of upstream `awawa-dev/HyperHDR`. Changing the visibility or the profile is a protected change that needs owner approval.

How pages 15–17 apply here:
- **Full rules, per-PR F review.** Pages 15–17 apply in full, and every PR gets F review. F also reviews every release candidate in full before installation or release.
- **Restricted channel.** Use GitHub private vulnerability reporting on this fork. Flaws in upstream code follow the upstream project's policy: report them privately to the upstream maintainers. The owner relays them; agents never post upstream.
- **Outside PRs.** These are PRs whose head is in another repository or whose author is not the owner. They never run on self-hosted runners or with secrets, and no `pull_request_target` workflow checks out PR code. Agents act only on issues and PRs the owner has approved (AGENT_POLICY.md §7).
- **Upstream merges.** Syncs from `awawa-dev/HyperHDR` are reviewed by D and F like dependency updates.
- **Possible second profile.** If this fork publishes release artifacts for other people to install, the "self-hosted software others install" rules become release criteria as well. That would mean secure defaults, published checksums or signatures, and advisories with fixes. This is open owner question 6.

**Workflow check (2026-11-20, from `.github/workflows/`)**

| Requirement | Result |
|---|---|
| No `pull_request_target` workflows | Pass |
| `rk3588-board.yml` is `workflow_dispatch` only, on the self-hosted runner | Pass |
| SignPath secrets are skipped for `pull_request` | Pass |
| `publish-daily-development-build.yml` runs only in `awawa-dev/HyperHDR` | Pass |
| Inherited upstream publishing on this fork | `upload-to-github-pages.yml` runs on any tag push here and tries to deploy to upstream's Pages repository, using a secret this fork doesn't have. Recorded as SF-006 |

The profile lists **intended** and **observed** facts separately, and the security reviewer (F) maps every finding to a profile below. Anything not yet confirmed is marked **Unknown** and is raised as a PM question. Environment-dependent security approval stays blocked until it is resolved. Credentials, hostnames, IP addresses, MAC addresses and usernames are deliberately left out; record them only in restricted owner notes.

Sources: `docs/rk3588-development-results.md`, `docs/rk3588-hardware-validation.md`, `docs/rk3588-mainline-review.md`, `docs/rk3588-board-runner.md`, `cmake/linux/service/hyperhdr@.service`, `sources/base/schema/*.json`, `.github/workflows/*.yml`.

## P1: Production (R1 target): Orange Pi 5 Plus, native

| Area | Intended | Observed (source) |
|---|---|---|
| Hardware | Orange Pi 5 Plus (RK3588), native HDMI-RX | Same (hardware-validation docs) |
| OS / arch | Debian 13 (Trixie) arm64, Armbian | Armbian 26.11.0, Debian 13 arm64 (development-results) |
| Kernel | Mainline with the HDMI-RX V4L2 driver | 6.18.44 (development-results). Earlier capture was confirmed on Armbian 26.8.3 |
| Topology | Single host on a trusted home LAN. Not a container, not cloud | Unknown: whether the LAN is segmented from untrusted devices or guests |
| Runtime | HyperHDR from the project `.deb` (Qt 6) | A native build was verified with GCC 14.2.0, CMake 3.31.6 and Qt 6.8.2. The installed-package path for R1 is not yet accepted (#27) |
| Service identity | systemd `hyperhdr@<user>.service`, running as an unprivileged login user (`User=%i`) | Unit file confirmed in the repository. **Unknown**: which user, its group memberships (`video` for `/dev/video*`), and whether it has sudo |
| Service hardening | — | **None** in the unit file: no `NoNewPrivileges`, `ProtectSystem`, `PrivateTmp`, capability bounding or `DeviceAllow` (security plan, finding SF-002) |
| Network exposure (listening) | Web UI and API on the LAN only | Defaults from the schema: HTTP **8090**, HTTPS **8092**, JSON API **19444**, Flatbuffers **19400**. Bind address, firewall and mDNS/SSDP exposure are **Unknown** |
| Network (outbound) | DDP over UDP **4048** (or WLED UDP 21324) to the ESP32/WLED on the LAN | Same (benchmark docs). DDP is unauthenticated and unencrypted by protocol design |
| Authentication | API auth on. Admin auth required for LAN clients | Defaults: `apiAuth=true`, `localApiAuth=false` (LAN clients use the API **without auth**), `localAdminAuth=true`, `internetAccessAPI=false`, `ipWhitelist=[]`. The configured values on the target are **Unknown** |
| Data sensitivity | Captured HDMI frames may show private content; settings DB; API tokens | Frames are processed in memory. Evidence collection redacts identifiers (`scripts/lib/redact.sh`) |
| Secrets | HyperHDR admin password and API tokens in the user's settings DB | Location and permissions of the settings DB on the target are **Unknown** |
| Filesystem | `/usr/bin/hyperhdr`, `/usr/share/hyperhdr`, config in the service user's home | Package layout checked by `tests/packaging/check-deb.sh` |
| Devices | `/dev/video*` for the native receiver, identified by driver/bus rather than a hardcoded node | Per the acceptance procedure |
| Install / update | `dpkg -i` of a version-pinned `.deb`. Settings are backed up before install | Procedure in `docs/rk3588-acceptance-run.md`. Package signing and checksum verification are **Unknown** (SF-004) |
| Logging | journald | Retention is Unknown |
| Backup / rollback | Settings backup plus reinstall of the previous `.deb` | Documented, but the rollback has **not been proven** on the target (R1 criterion C5) |

## P2: Hardware CI runner (#28): same board, self-hosted GitHub runner

| Area | Intended | Observed |
|---|---|---|
| Identity | Repository-scoped runner; non-root, without passwordless sudo (enforced by `board-preflight.sh`) | Not provisioned (#28) |
| Trigger | `workflow_dispatch` only, `master` only, through the `rk3588-hil` environment with required reviewers | Enforced by the workflow and `tests/ci/board-workflow-policy-test.py`. Whether required reviewers are configured on the environment is **Unknown** |
| Risk | Runner code runs on the production board | The board is shared with P1, so a compromised workflow reaches the production host |

## P3: Hosted CI (GitHub Actions)

- **Intended:** Linux only. `ubuntu-26.04` and `ubuntu-26.04-arm` runners, and Docker build images for Debian 13 (trixie) arm64 and amd64. The optional compatibility images (Debian, Ubuntu, Fedora, Arch) run by manual dispatch only.
- **Observed (2026-11-21):** `master` still runs the macOS and Windows jobs and the 16-entry Linux matrix until PR #82 merges. Windows and macOS are outside this fork's supported scope (`docs/fork-scope.md`).
- Actions are pinned by version tag (`actions/checkout@v7.0.1`), **not by commit SHA**.
- Default `GITHUB_TOKEN` permissions in `push-pull.yml`: **intended** `contents: read` at workflow level (PR #82). **Observed** on `master`: no explicit block, so the repository default applies, which is Unknown (SF-003).
- Fork PRs run CI through the `pull_request` event.

## P4: Development and agent sessions

- Claude Code cloud containers: ephemeral, Linux x86_64, with GitHub access through the owner's account (agents post as the owner).
- No production credentials and no network path to the target LAN.
- Local tests here prove nothing about P1 safety.

## Open PM questions (Unknowns that block environment-dependent approval)

1. On P1: the service user, its groups, and whether it has sudo.
2. On P1: listening addresses for 8090, 8092, 19444 and 19400; firewall state; whether the LAN contains untrusted devices.
3. On P1: the configured values of `localApiAuth`, `localAdminAuth` and `ipWhitelist`.
4. How `.deb` integrity is verified on install (checksum or signature).
5. Whether the `rk3588-hil` environment has required reviewers configured.
6. Whether this fork publishes release artifacts (GitHub Releases or packages) for other people to install. If it does, the "self-hosted software others install" rules apply too, and SF-001's default (`localApiAuth=false`) becomes a release blocker.
7. Whether private vulnerability reporting is enabled on this fork (Settings → Code security).
