# Project plan

Policy revision: `AP-2026.11.21-7` · PM Control: issue #74 · Priority: **low** (16-hour coordination cycle).

## Charter

**Fork objectives** (owner, 2026-11-20 and 2026-11-21; issue #81; `docs/fork-scope.md` after PR #82):
1. The Orange Pi 5 Plus (RK3588) with Armbian Debian 13 arm64 is the primary supported deployment and release target.
2. Linux x86-64 stays supported for development, automated regression testing and CI.
3. Windows and macOS are outside this fork's supported development, testing and release scope.
4. Other Linux distributions are not routine release targets; compatibility testing only on manual request.
5. Windows and macOS source and build-system support remain intact, so support can be restored.
6. All RK3588-relevant functional, sanitizer, security, regression and release-acceptance testing remains mandatory.


Deliver the smallest stable, measurable release of **Orange Pi 5 Plus (RK3588) native HDMI input → HyperHDR CPU processing → DDP → ESP32/WLED LEDs**, optimizing for verified end-to-end behaviour and preserving every proven colour, latency, reliability and security property. Parent roadmap: #7.

Charter fields (edition 1.0 §4):
- **Users:** the owner, as operator of the Orange Pi 5 Plus.
- **Intended capability:** native HDMI capture, CPU processing and DDP output to WLED.
- **Success measures:** the R1 criteria below, accepted on one pinned SHA.
- **Scope:** R1. **Non-goals:** the deferred R2 items.
- **Supported platforms:** Orange Pi 5 Plus with Debian 13 arm64 (Armbian) for acceptance; hosted CI on x86_64 and arm64 for development.
- **Quality attributes:** memory safety, colour fidelity, latency, recovery.
- **Accountable owner:** Hans5849.
- **Roadmap horizon:** R1, then R2.
- **PM review capacity:** about 5 decisions per batch, and at most 3 candidates waiting.

**Increments:** one week (two for larger integrations). **Priority bands:** P0 critical defects and release blockers · P1 increment-critical · P2 normal backlog · P3 proposals.

**Work states:** Backlog, Ready, In progress, In review, Needs validation, Done. A blockage records its cause, owner and unblock condition. A merged issue that still awaits acceptance stays in Needs validation. Labels confer no authority.

**Definition of Ready**
- recorded authority;
- an understandable outcome with testable acceptance;
- scope and non-goals;
- dependencies and target branch;
- risk tier and allowed actions;
- tests and operational validation;
- rollback considerations and a review estimate.

**Definition of Done**
- criterion-specific evidence, accepted by its authorized owner;
- current required checks, and current D and F records;
- required operational validation;
- security and compatibility concerns resolved;
- linked records updated.

## Project (all releases)

| Scope and baseline | Accepted weight | Completion | Awaiting acceptance |
|---|---|---|---|
| Project: R1 (R2 not yet scoped), baseline PROPOSED | Not baselined | Not baselined | See #74 |

Each criterion counts once in the project total, under one primary component. Aggregate weights, never percentages. Show the numerator, denominator and baseline revision next to every percentage.

## Release R1: RK3588 CPU-only HDMI → WLED

Baseline status: **PROPOSED, not baselined** (owner decision D-002). Until approved, the dashboard shows "Not baselined", not a percentage.

Each acceptance criterion counts once under one component. Weights are proposed as equal per criterion (weight 1).

| Component | Issues | Acceptance criteria (weight 1 each) | Weight |
|---|---|---|---|
| C1 End-to-end hardware path | #8 | Native receiver identified by driver/bus; live BGR24 preview at 1080p; supported 4K timings; DDP received by WLED with correct LED order and colour | 4 |
| C2 Image fidelity | #10 | Orientation and RGB order correct; full/limited range correct; colour reference suite passes on the board | 3 |
| C3 Benchmark baseline | #9 | Reproducible CPU-only baseline published with metadata; capture and output rates independently measured | 2 |
| C4 Reliability and recovery | #11, #19 | No-source/reconnect recovery verified (incl. during retry backoff); 30-minute stability run | 2 |
| C5 Install and diagnostics | #27 | Diagnostics suite passes on the installed package; verified, restorable `.deb` install with settings backup and rollback | 2 |
| C6 Hardware CI runner | #28 | Dedicated runner provisioned and `rk3588-board.yml` passes on trusted `master` | 1 |
| C7 Decoder memory safety | #61, #64, #68, #71 | Vectorised decoder bounds fixed and merged with sanitizer tests (#61/#64); remaining unaligned reads fixed (#68); performance tradeoff documented (#71) | 3 |
| C8 Delivery hygiene | #18 | Release documentation and CI evidence current for the release candidate | 1 |
| **Total** | | | **18** |

**Release requirements (CI).** The tag's exact SHA must pass:
- `RepositoryPolicy`;
- `CaptureTests` on x86_64 and arm64 (ASan/UBSan);
- CodeQL;
- the Debian 13 arm64 and amd64 packages, with the install and CLI smoke test;
- `Fork CI guard`.

These are the `publish` gates of PR #82. Windows and macOS jobs are not release requirements. Hosted CI never replaces hardware acceptance (`docs/rk3588-acceptance-run.md`).

Release stop condition: every criterion has evidence on the **same pinned candidate SHA**, with rollback proven, and the owner gives GO.

## Deferred (R2, not in R1 scope)

#12 CPU/NEON, #13 DMA-BUF, #14 RGA, #15 GPU, #16 NPU, #17 backend selection. Pursued only with on-board evidence and owner approval.

## Open decisions

See the Attention required table on #74 (D-001 policy adoption, D-002 baseline, D-003 activation, D-004 PR #72 supersession).

## Risk register

| ID | Cause → event → consequence | Likelihood / impact | Owner | Trigger | Mitigation / contingency | Status |
|---|---|---|---|---|---|---|
| RK-1 | Agents share the owner's admin identity → a misbehaving session merges or pushes to `master` → an unreviewed change lands | Low / High | Hans5849 | Any agent merge or `master` push | Merge delegation NONE; B disabled; instruction-level limits (D-006 2a); branch ruleset recommended (D-007) | Open |
| RK-2 | No verified lease → two writers edit the same workstream → conflicting or lost work | Medium (once B1 and B2 are active) / Medium | Hans5849 | Activation of B1/B2 | B stays disabled until a lease is verified (D-003) | Open |
| RK-3 | Hardware access delays → acceptance criteria can't be evidenced → R1 slips past the Sprint 1 deadline | High / Medium | Hans5849 | No operator session scheduled | Prepare Batch R and operator procedures; accept partial progress | Open |

## Change history

| Date | Change | Decision |
|---|---|---|
| 2026-11-20 | Plan created under the operating standard; R1 baseline proposed | Pending D-002 |
| 2026-11-21 | Aligned with consolidated edition 1.0: charter fields, Ready/Done definitions, states, priority bands, project row, risk register. Baseline unchanged | Pending D-002 |
