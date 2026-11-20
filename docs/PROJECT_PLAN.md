# Project plan

Policy revision: `AP-2026.11.20-2` · PM Control: issue #74 · Priority: **low** (16-hour coordination cycle).

## Charter

Deliver the smallest stable, measurable release of **Orange Pi 5 Plus (RK3588) native HDMI input → HyperHDR CPU processing → DDP → ESP32/WLED LEDs**, optimizing for verified end-to-end behaviour and preserving every proven colour, latency, reliability and security property. Parent roadmap: #7.

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

Release stop condition: every criterion has evidence on the **same pinned candidate SHA**, with rollback proven, and the owner gives GO.

## Deferred (R2, not in R1 scope)

#12 CPU/NEON, #13 DMA-BUF, #14 RGA, #15 GPU, #16 NPU, #17 backend selection. Pursued only with on-board evidence and owner approval.

## Open decisions

See the Attention required table on #74 (D-001 policy adoption, D-002 baseline, D-003 activation, D-004 PR #72 supersession).

## Change history

| Date | Change | Decision |
|---|---|---|
| 2026-11-20 | Plan created under the operating standard; R1 baseline proposed | Pending D-002 |
