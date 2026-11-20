# Resource limits

Policy revision: `AP-2026.11.20-3` · Checked: 2026-11-20 (setup) · Owner: Hans5849

Agents re-check these each coordinator run where they can, and mark anything they cannot verify **Unknown**. Spending, purchases or limit changes are PM review items; agents never buy capacity or skip required tests for lack of it.

| Limit | Allowance | Current usage | Remaining | Source | Checked | Alert threshold | Response |
|---|---|---|---|---|---|---|---|
| GitHub Actions minutes (hosted Linux, arm64, macOS) | Unknown (public repository: standard runners normally free; confirm) | Unknown | Unknown | Owner billing settings (not readable by agents) | 2026-11-20 | Any billing warning | Ask owner; reduce matrix reruns |
| Actions concurrency / queue | Unknown | Unknown | Unknown | Account plan | 2026-11-20 | Queue > 1 h | Deduplicate runs; one full matrix per head |
| Actions artifact storage and retention | Unknown | Unknown | Unknown | Repository settings | 2026-11-20 | Storage warning | Shorten retention via owner |
| Self-hosted RK3588 runner (#28) | 1 board, owner-gated | Not provisioned / Unknown | n/a | #28 | 2026-11-20 | Offline | Hardware work BLOCKED; continue hosted work |
| Claude Code scheduled-task runs | Coordinator + developer every 16 h (≈ 3 runs each per 48 h) | n/a | Unknown | Claude plan limits | 2026-11-20 | Run refused / quota | Report on #74; owner decides |
| Model and API quotas | Unknown | Unknown | Unknown | Claude plan | 2026-11-20 | Rate-limit errors | Stop, report |
| Owner review capacity | ≤ 3 PRs awaiting owner action; ~5 decisions per batch | See #74 | — | AGENT_WORKFLOW.md §6 | 2026-11-20 | Queue full | Prioritize repair and review |
| Operator / board time | Owner-scheduled | — | — | Owner | 2026-11-20 | — | Batch acceptance steps |
