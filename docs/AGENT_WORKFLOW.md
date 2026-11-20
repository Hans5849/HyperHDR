# Agent workflow: 16-hour cycle, execution and reporting

Policy revision: `AP-2026.11.20-3` · Authority rules: [AGENT_POLICY.md](AGENT_POLICY.md) · PM Control: issue #74 · Sprint coordination: issue #75.

The primary measure is **accepted deliverables** with traceable evidence, within the owner's review capacity. Merged PRs, commits and closed issues are activity, not progress.

## 1. Cadence (owner amendment 2026-11-20)

This project is low priority. The standard's four-hour rhythm is replaced by a **16-hour coordination cycle**. Everywhere the standard says "four-hour", read "16-hour". The cycle is a coordination cadence, not a requirement to produce code; "no approved work" is a valid no-op.

| Report | Cadence |
|---|---|
| Cycle report | Every coordinator run (every 16 hours) |
| Daily summary | Folded into the first cycle report of each UTC day |
| Weekly / increment report | First cycle report on or after Monday 00:00 UTC |

Planning: outcomes are planned per two-week increment.

## 2. Roles and persistent sessions

| Role | Assignment |
|---|---|
| A Coordinator | Reconciles prior work, PM notices and decisions; maintains the #74 body; issues bounded work orders. |
| B Developer | Claims authorized work; implements and repairs CI; tests and reports. Starts 45 minutes after A. |
| D Reviewer | Reviews PRs it did not author; records findings and exact-candidate evidence on the PR. Never reviews its own work. Read-only; uses no writer slot. |
| F Security reviewer | Reviews code, dependencies and delivery controls against [INSTALLATION_PROFILE.md](INSTALLATION_PROFILE.md); posts the security gate record per [SECURITY_PLAN.md](SECURITY_PLAN.md). Read and comment only; writes no remediation; uses no writer slot. |
| C Integration, E Go/no-go | **Omitted** for Sprint 1 (no approved integration candidate or release candidate). Omitting a role never omits its checks: without C, B builds any integration preview under a work order and D and F verify it; without E, A compiles the go/no-go package for the owner. Adding either needs an explicit schedule, scope and an inventory entry (§12). |

A, B, D and F are required roles. B, D and F are always separate sessions. Each scheduled role has **one persistent Claude Code session**, created with `create_session` (source `https://github.com/Hans5849/HyperHDR`, title `Sprint 1: HyperHDR <role> (scheduled)`, tag `hyperhdr-sprint-20261120`). Its scheduled task resumes that session through `persistent_session_id`. **Fresh-session triggers are never used.** Every run re-verifies checkout, GitHub access and durable context (GitHub records, not session memory) before any write. If the platform cannot meet this contract, the role reports setup BLOCKED on #74 instead of substituting a fresh session.

## 3. Schedule

Cron cannot express a 16-hour interval directly, so the cycle repeats every 48 hours on day-of-month parity. Every cron is prefixed `CRON_TZ=America/New_York`.

| Role | Odd days (New York) | Even days (New York) | Crons |
|---|---|---|---|
| A Coordinator | 00:10, 16:10 | 08:10 | `10 0,16 1-31/2 * *` · `10 8 2-30/2 * *` |
| B Developer | 00:55, 16:55 | 08:55 | `55 0,16 1-31/2 * *` · `55 8 2-30/2 * *` |
| D Reviewer | 01:15, 17:15 | 09:15 | `15 1,17 1-31/2 * *` · `15 9 2-30/2 * *` |
| F Security reviewer | 01:45, 17:45 | 09:45 | `45 1,17 1-31/2 * *` · `45 9 2-30/2 * *` |

Gaps are 16 hours, except one 8-hour gap after the 31st of a month (and after 29 February). Sprint 1 runs entirely in EST (UTC−05:00); no daylight-saving change falls inside it. A schedule is not a dependency: work-order freshness, claims, gates and resource limits are enforced regardless of timing.

Rules: record scheduled and actual times in UTC and America/New_York. De-duplicate repeated triggers by run ID (`coord-`, `dev-`, `rev-`, `sec-` plus the scheduled UTC time as `YYYYMMDDTHHMMZ`). A delayed or missed run never implies a completed handoff or permission to start. Mark the dashboard **STALE** when a scheduled coordinator report is missing. Timing never implies review completion: A tracks each candidate's D and F status by head SHA, profile revision and age on #74. B continues other authorized work while a review waits; waiting never becomes a pass. If the queue exceeds what the scheduled F runs can clear, A requests an owner-authorized extra F run in F's session, and it is added to the inventory (§12). A missed or failed F run is alerted on #74 and leaves the gate unmet.

Stop procedure: the one-time shutdown task (§12). Changing schedules, the deadline or the inventory is an owner action; untracked schedules are prohibited.

## 4. Work-order contract

The coordinator posts each work order as a comment on the coordination issue (#75) and links it from the #74 work-order slot. An order is READY only when every field is verified:

run ID · repository · release and approved issue · objective, scope and non-goals · target branch, base SHA (full) and policy revision · dependencies and file set · allowed and prohibited actions · `OWNER APPROVAL:` link covering the scope · required tests (registry IDs) and acceptance · security relevance and installation-profile revision · D and F reviewers · fallback tasks · execution, CI-watch and retry budgets · created time, claim deadline (after developer start, within 60 minutes) and execution deadline (before the next coordinator slot) · lease mechanism · report destinations.

A missing field makes the order DRAFT or BLOCKED with the reason. While the activation gate (AGENT_POLICY.md §6) is closed, the coordinator publishes no READY order and records `Work-order slot: NONE READY (activation gate closed)`.

The developer rereads live state and authorization before claiming. A missing, expired, superseded or inconsistent order is a safe no-op with a diagnostic. A base or policy change requires reconciliation before any write.

## 5. States, claims and leases

Order states: DRAFT → READY → CLAIMED → RUNNING → REPORTED → RECONCILED, with BLOCKED, FAILED, EXPIRED and CANCELLED outcomes.

A durable lease records holder, workstream, heartbeat, expiry and a fencing token. GitHub `CLAIM` and `RELEASE` comments on the coordination issue (#75) are the visible audit trail; **labels and comments alone are not an atomic lock**, so until an exclusive lease mechanism is named and validated (§11), the activation gate stays closed. One writer per conflicting workstream; never share a writable worktree. Lease expiry does not prove the old worker stopped: revoke or fence before reassigning. Retries are idempotent.

Budgets: finite execution, CI-watch and retry budgets; checkpoint before the next coordinator slot. Every role checks the sprint cutoff and shutdown state (§12) before any write. On CI or hardware delay, keep pending status and take only approved independent fallback work. On authentication failure, stale state, repeated test failure or exhausted budget, report and stop.

## 6. Execution loop (developer)

Capacity: one primary implementation plus one independent fallback workstream (D and F review read-only and use no writer slot); at most **three PRs awaiting owner action**. When the queue is full, prioritize repair, review and acceptance preparation.

1. **Preflight and claim:** read policy, live issues, PRs, heads, checks and claims. Verify checkout, credentials and dependencies. Acquire the lease; post `CLAIM`.
2. **Implement:** issue-specific `claude/<topic>` branch and isolated worktree; one focused PR per issue; declare dependency order.
3. **Prove the fix:** add a regression test; for defects, show it fails on the unfixed code (or a controlled mutant) in a disposable worktree, then passes.
4. **Review and verify:** D and F independently review the exact candidate. Run required CI and the local suites in TEST_PLAN.md. Any push invalidates candidate-specific gates, including F's.
5. **Integrate or defer:** apply every gate in §7. Without authority or evidence, leave a reviewable PR and record the blocker.
6. **Report and release:** post `RELEASE` with PR link, head SHA, files, tests and results, unverified scenarios, blockers and next owner.

Fallback order: safe partial work on the current issue → approved independent work in the same increment → the Ready queue. Never jump repositories or invent features. Use `Refs #N` while acceptance is outstanding.

## 7. Per-PR gates

Merging requires a current delegation (none exists; AGENT_POLICY.md §5). Any push, rebase or changed merge candidate requires renewed D and F review. Every PR handed to the owner carries this gate table, re-checked immediately before handoff:

| Gate | Evidence |
|---|---|
| Authority and scope | Approval link and policy revision; no unauthorized scope, production, tag, release or security-policy change |
| Independent review | Non-author review of the full current diff; every finding resolved or dispositioned |
| Exact-candidate CI | All required checks green on the exact head SHA (pending, cancelled, skipped-required or old-SHA ≠ pass) |
| Local and platform suites | TEST_PLAN.md required suites on the same head in a clean worktree; x86_64 and arm64 |
| Security gate (F) | F's `SECURITY GATE` record for the exact head SHA and current profile revision (SECURITY_PLAN.md §4); a missing or stale record is not a pass. No exposed secrets, unresolved blockers or weakened tests |
| Dependencies | Predecessors merged in order; correct base |
| Race protection | Live head, base, policy and checks re-read; head SHA recorded for an expected-head merge |

## 8. Release acceptance

Implemented, CI-passed, reviewed, merged, operationally validated, owner-accepted, deployed and released are separate states. Acceptance batches: **R** (read-only, disposable, pinned to the final SHA) and **I** (real installation on the Orange Pi; owner approval required; operator performs physical steps per `docs/rk3588-acceptance-run.md`). Synthetic or CI success is never presented as real-machine acceptance. Batch I also requires a verified installation profile for each affected machine. The final SHA or artifact digest needs its own F disposition, and the go/no-go package includes F's final-candidate security disposition. A missing mandatory gate is NO-GO. Tags and release publication need specific owner authority.

## 9. Notices and PM review items

Before opening an issue, check for an existing one. After opening, post a notice comment on #74 in the same run (for a sensitive security finding, a redacted notice only; detail goes to the restricted channel, SECURITY_PLAN.md §6):

`N-### | issue opened | #N, author, created (UTC) | reason, release/component | owner, next action | status: New | PM decision needed: Yes/No`

Track delivery (Pending / Posted / Failed) separately from acknowledgment, decision state (Awaiting decision, Approved, Rejected, Deferred, Superseded) and execution state (Not started, In progress, Blocked, Verified, Cancelled). Never report an undelivered notice as seen. Each coordinator run compares issues opened since the last checkpoint with posted notices and fills gaps.

Every PM review item has: **Context and evidence**, **Options** (alternatives, tradeoffs, recommendation, consequence of deferral), **Comments** (PM questions, conditions, rationale with author/date) and **Status** (delivery, acknowledgment, decision owner and due date, approval link, execution owner/status, verification). Acknowledgment is not authorization.

## 10. Coordinator run and dashboard

Each coordinator run:

1. Record the run ID and the scheduled and actual times. If #74 already has a report for this run ID, exit (duplicate trigger). If the previous scheduled report is missing, mark that interval STALE.
2. Reconcile: `git fetch origin --prune`; read `master`, open issues and PRs with exact head SHAs, CI on those heads, reviews, prior orders, CLAIM/RELEASE comments and reports. Classify prior orders.
3. Post missing notices (§9).
4. Publish at most one work order (§4), only if the activation gate is open.
5. Update the #74 body: as-of time, last successful run, Releases, Components, Attention required, Notices index, work-order slot, gate state. Use full SHAs and real links; a missing link is a visible evidence gap. Show "Not baselined" rather than a percentage without an approved denominator.
6. Post a cycle report comment: run ID; base → current SHA; issue/PR links; delivery; CI evidence on exact heads; implementation vs acceptance; accepted weight gained; blockers with owners and unblock conditions; notices and their delivery state; three next actions. Post nothing new when nothing changed beyond a one-line no-change report.

Completion = 100 × accepted weight / approved weight, aggregated over weights (never averaged percentages).

## 11. Validation matrix (required before the activation gate opens)

- [ ] Policy merged; AGENTS.md and CLAUDE.md present, cross-linked, same revision (`tests/ci/agent-entrypoints-test.py` green on `master`).
- [ ] R1 baseline approved (D-002).
- [ ] An exclusive lease mechanism is named and tested (two concurrent claimers: one wins, the other no-ops).
- [ ] Missing/expired order → developer no-ops with a diagnostic.
- [ ] Duplicate trigger → second run no-ops by run ID.
- [ ] Stale base SHA → no write; reconciliation reported.
- [ ] Missed coordinator run → dashboard marked STALE.
- [ ] Denied write / auth failure → stop with actionable report.
- [ ] Two consecutive clean read-only 16-hour cycles with accurate dashboard and links.
- [ ] Security drills: a missing F record, stale SHA, stale profile revision, expired owner exception, failed scanner and a restricted finding each block the affected gate and reach #74 correctly.

## 12. Sprint 1: deadline, shutdown and setup receipt

- Sprint `hyperhdr-sprint-20261120`. Deadline **2026-12-12 23:59 America/New_York** (UTC−05:00). Cutoff and shutdown: **2026-12-12 23:14 America/New_York = 2026-12-13T04:14:00Z**.
- From the cutoff onward: no new work orders, claims or branch writes. A one-time shutdown task, bound to a persistent control session, does the following:
  1. Sets sprint state STOPPING.
  2. Disables every task in the inventory with `update_trigger enabled=false` and verifies each by readback. This includes any optional or later-added task.
  3. Asks active holders to checkpoint and RELEASE.
  4. Posts the final handoff on #74, linked from #75.
  Disabling a trigger does not stop a running session. A disable failure, or an active worker that cannot stop, is an owner-action blocker and is never reported as full shutdown.
- The task inventory (role, session ID, trigger IDs, schedules) lives in the **setup receipt** comment on #74. That comment is the durable inventory the shutdown task reads. Any task added later must be added to the receipt before it is enabled.
- Role activation: each role acts only if the latest SETUP RECEIPT on #74 lists its letter on the `Active roles:` line. Otherwise it verifies access and exits with no writes. Development (B) is not activated until every launch check (page 17 of the standard) passes: security records confirmed by the owner, F least-privilege readback, a restricted channel that F can file in, review flow, and shutdown tested.
- Restricted reporting channel: GitHub private security advisories on `Hans5849/HyperHDR`. Security response owner and shutdown fallback: Hans5849.
- Last scheduled runs before the cutoff: B 2026-12-12 08:55, D 09:15 and F 09:45 (America/New_York). Candidates pushed after the final F run are handed off as awaiting the security gate and do not merge in this sprint.
- The final handoff also lists every open finding by ID (sensitive detail redacted), each open PR's D/F gate status and SHA, active owner exceptions with expiry, and candidates not yet reviewed, each with a follow-up owner. The shutdown posts it as a comment; only A updates the #74 body. A restart needs owner direction, a re-verified profile revision, fresh F records and a new verified shutdown task.
- Resuming after the deadline requires recorded owner direction.
