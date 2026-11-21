# Resource limits

Policy revision: `AP-2026.11.21-7` · Checked: 2026-11-20 (setup) · Owner: Hans5849

Agents re-check these each coordinator run where they can, and mark anything they cannot verify **Unknown**. Spending, purchases or limit changes are PM review items; agents never buy capacity or skip required tests for lack of it.

| Limit | Allowance | Current usage | Remaining | Source | Checked | Alert threshold | Response |
|---|---|---|---|---|---|---|---|
| GitHub Actions minutes (hosted Linux x86_64 and arm64; Windows and macOS removed by PR #82) | Unknown (public repository: standard runners normally free; confirm) | Unknown | Unknown | Owner billing settings (not readable by agents) | 2026-11-20 | Any billing warning | Ask owner; reduce matrix reruns |
| Actions concurrency / queue | Unknown | Unknown | Unknown | Account plan | 2026-11-20 | Queue > 1 h | Deduplicate runs; one full matrix per head |
| Actions artifact storage and retention | Unknown | Unknown | Unknown | Repository settings | 2026-11-20 | Storage warning | Shorten retention via owner |
| Self-hosted RK3588 runner (#28) | 1 board, owner-gated | Not provisioned / Unknown | n/a | #28 | 2026-11-20 | Offline | Hardware work BLOCKED; continue hosted work |
| Claude Code scheduled-task runs | Coordinator + developer every 16 h (≈ 3 runs each per 48 h) | n/a | Unknown | Claude plan limits | 2026-11-20 | Run refused / quota | Report on #74; owner decides |
| Model and API quotas | Unknown | Unknown | Unknown | Claude plan | 2026-11-20 | Rate-limit errors | Stop, report |
| Owner review capacity | ≤ 3 PRs awaiting owner action; ~5 decisions per batch | See #74 | — | AGENT_WORKFLOW.md §6 | 2026-11-20 | Queue full | Prioritize repair and review |
| Operator / board time | Owner-scheduled | — | — | Owner | 2026-11-20 | — | Batch acceptance steps |

## Campaign budget (Sprint 1, proposed; needs owner approval, D-008)

| Budget | Proposed limit | Notes |
|---|---|---|
| Writers | 2 (B1, B2) | Disabled until READY and D-003 |
| Distinct candidates waiting on D, F or PM | 3 | Edition 1.0 §7 |
| Review concurrency | 1 D and 1 F at a time | Their sessions are serial |
| On-demand review dispatches | ≤ 4 per day per role | On top of the 16-hour recovery sweeps |
| Dispatch retries / repair rounds | 2 transient retries per request; 2 repair rounds per candidate | Then escalate on #74 |
| Writer runtime | Until the next A slot, minus 15 min to checkpoint | |
| CI watch interval | ≤ 45 min per candidate | Then checkpoint and release |
| Full CI matrix runs | ≤ 1 per candidate head | No duplicate equivalent runs |
| Model usage / spend | Unknown (the plan allowance isn't readable by agents) | The owner sets warning and stop thresholds |
| PM review time | About 30 min per 16-hour cycle | |

**Thresholds.** Exhaustion never silently lowers model depth or test coverage. On a stop threshold, A holds new orders and escalates on #74.

## Measured CI baseline

Full push run before PR #82 ([run 44812839215](https://github.com/Hans5849/HyperHDR/actions/runs/44812839215)): 23 jobs and 98.8 runner-minutes (macOS 16.8, Windows 6.6, Linux matrix 53.3, CodeQL 15.2, sanitizers 5.9).

Estimated after PR #82:
- code change: 7 jobs, about 26 minutes;
- documentation-only change: 2 jobs, about 1 minute;
- release tag: 9 jobs, about 31 minutes.

**Billing assumptions.** This is a public repository, so standard GitHub-hosted runners are free. On a private repository, GitHub bills Linux at 1×, Windows at 2× and macOS at 10× per minute; the removed macOS jobs (16.8 min) and Windows job (6.6 min) would have cost the most there. Linux arm64 standard runners are free for public repositories. The account's actual plan and limits are Unknown to agents (owner to confirm).

## Current usage

Unknown. No development runs have happened yet. A records usage per cycle report, where measurable.
