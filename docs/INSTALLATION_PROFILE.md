# Installation profile

Policy revision: `AP-2026.11.20-3` · Profile revision: `IP-1` · Accountable owner: Hans5849 · Last verified: 2026-11-20 (from repository docs and code only; **not** re-observed on the target)

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

- Platforms: `ubuntu-26.04` and `ubuntu-26.04-arm`, macOS, and Docker build images (Debian, Ubuntu, Fedora, Arch).
- Actions are pinned by version tag (`actions/checkout@v7.0.1`), **not by commit SHA**.
- Default `GITHUB_TOKEN` permissions in `push-pull.yml` are **Unknown**: no explicit `permissions:` block (SF-003).
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
