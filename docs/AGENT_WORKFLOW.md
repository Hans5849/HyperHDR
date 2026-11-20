# Agent workflow: 16-hour cycle, execution and reporting

Policy revision: `AP-2026.11.20-1` · Authority rules: [AGENT_POLICY.md](AGENT_POLICY.md) · PM Control: issue #74.

The primary measure is **accepted deliverables** with traceable evidence, within the owner's review capacity. Merged PRs, commits and closed issues are activity, not progress.

## 1. Cadence (owner amendment 2026-11-20)

This project is low priority. The standard's four-hour rhythm is replaced by a **16-hour coordination cycle**. Everywhere the standard says "four-hour", read "16-hour". The cycle is a coordination cadence, not a requirement to produce code; "no approved work" is a valid no-op.

| Report | Cadence |
|---|---|
| Cycle report | Every coordinator run (every 16 hours) |
| Daily summary | Folded into the first cycle report of each UTC day |
| Weekly / increment report | First cycle report on or after Monday 00:00 UTC |

Planning: outcomes are planned per two-week increment.

## 2. Roles on Claude

Separate Claude Code cloud sessions fill the roles: a **coordinator** task and, 45 minutes later, a **developer** task. Each firing starts a fresh session, which must clone the repository and rebuild state from GitHub before any write. Reviewers are separate sessions named in the work order.

## 3. Schedule

Cron cannot express a 16-hour interval directly, so the cycle repeats every 48 hours on day-of-month parity, in **UTC** (no daylight-saving drift).

| Role | Odd days of month (UTC) | Even days of month (UTC) | Cron |
|---|---|---|---|
| Coordinator | 02:10, 18:10 | 10:10 | `10 2,18 1-31/2 * *` and `10 10 2-30/2 * *` |
| Developer | 02:55, 18:55 | 10:55 | `55 2,18 1-31/2 * *` and `55 10 2-30/2 * *` |

Gaps are exactly 16 hours except once after the 31st of a month and after 29 February, when the gap is 8 hours. Run IDs de-duplicate, so the short gap is harmless. Reports show times in UTC and America/New_York.

Rules: record scheduled and actual times in UTC. De-duplicate repeated triggers by run ID (`coord-YYYYMMDDTHHMMZ`, `dev-YYYYMMDDTHHMMZ`, from the scheduled UTC time). A delayed or missed run never implies a completed handoff or permission to start. Mark the dashboard **STALE** when a scheduled coordinator report is missing.

Stop procedure: disable the scheduled tasks in Claude Code (claude.ai → Code → Scheduled tasks). Changing the schedule is an owner action.

## 4. Work-order contract

The coordinator posts each work order as a comment on #74. An order is READY only when every field is verified:

run ID · repository · release and approved issue · objective, scope and non-goals · target branch, base SHA (full) and policy revision · dependencies and file set · allowed and prohibited actions · `OWNER APPROVAL:` link covering the scope · required tests (registry IDs) and acceptance · named independent reviewer · fallback tasks · execution, CI-watch and retry budgets · created time, claim deadline (after developer start, within 60 minutes) and execution deadline (before the next coordinator slot) · lease mechanism · report destinations.

A missing field makes the order DRAFT or BLOCKED with the reason. While the activation gate (AGENT_POLICY.md §6) is closed, the coordinator publishes no READY order and records `Work-order slot: NONE READY (activation gate closed)`.

The developer rereads live state and authorization before claiming. A missing, expired, superseded or inconsistent order is a safe no-op with a diagnostic. A base or policy change requires reconciliation before any write.

## 5. States, claims and leases

Order states: DRAFT → READY → CLAIMED → RUNNING → REPORTED → RECONCILED, with BLOCKED, FAILED, EXPIRED and CANCELLED outcomes.

A durable lease records holder, workstream, heartbeat, expiry and a fencing token. GitHub `CLAIM` and `RELEASE` comments on #74 are the visible audit trail; **labels and comments alone are not an atomic lock**, so until an exclusive lease mechanism is named and validated (§11), the activation gate stays closed. One writer per conflicting workstream; never share a writable worktree. Lease expiry does not prove the old worker stopped: revoke or fence before reassigning. Retries are idempotent.

Budgets: finite execution, CI-watch and retry budgets; checkpoint before the next coordinator slot. On CI or hardware delay, keep pending status and take only approved independent fallback work. On authentication failure, stale state, repeated test failure or exhausted budget, report and stop.

## 6. Execution loop (developer)

Capacity: one primary implementation plus one independent review or fallback workstream; at most **three PRs awaiting owner action**. When the queue is full, prioritize repair, review and acceptance preparation.

1. **Preflight and claim:** read policy, live issues, PRs, heads, checks and claims. Verify checkout, credentials and dependencies. Acquire the lease; post `CLAIM`.
2. **Implement:** issue-specific `claude/<topic>` branch and isolated worktree; one focused PR per issue; declare dependency order.
3. **Prove the fix:** add a regression test; for defects, show it fails on the unfixed code (or a controlled mutant) in a disposable worktree, then passes.
4. **Review and verify:** a different session reviews the exact candidate. Run required CI and the local suites in TEST_PLAN.md. Any push invalidates candidate-specific gates.
5. **Integrate or defer:** apply every gate in §7. Without authority or evidence, leave a reviewable PR and record the blocker.
6. **Report and release:** post `RELEASE` with PR link, head SHA, files, tests and results, unverified scenarios, blockers and next owner.

Fallback order: safe partial work on the current issue → approved independent work in the same increment → the Ready queue. Never jump repositories or invent features. Use `Refs #N` while acceptance is outstanding.

## 7. Per-PR gates

Merging requires a current delegation (none exists; AGENT_POLICY.md §5). Every PR handed to the owner carries this gate table, re-checked immediately before handoff:

| Gate | Evidence |
|---|---|
| Authority and scope | Approval link and policy revision; no unauthorized scope, production, tag, release or security-policy change |
| Independent review | Non-author review of the full current diff; every finding resolved or dispositioned |
| Exact-candidate CI | All required checks green on the exact head SHA (pending, cancelled, skipped-required or old-SHA ≠ pass) |
| Local and platform suites | TEST_PLAN.md required suites on the same head in a clean worktree; x86_64 and arm64 |
| Security and test integrity | No secrets; no test disabled or weakened |
| Dependencies | Predecessors merged in order; correct base |
| Race protection | Live head, base, policy and checks re-read; head SHA recorded for an expected-head merge |

## 8. Release acceptance

Implemented, CI-passed, reviewed, merged, operationally validated, owner-accepted, deployed and released are separate states. Acceptance batches: **R** (read-only, disposable, pinned to the final SHA) and **I** (real installation on the Orange Pi; owner approval required; operator performs physical steps per `docs/rk3588-acceptance-run.md`). Synthetic or CI success is never presented as real-machine acceptance. A missing mandatory gate is NO-GO. Tags and release publication need specific owner authority.

## 9. Notices and PM review items

Before opening an issue, check for an existing one. After opening, post a notice comment on #74 in the same run:

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
