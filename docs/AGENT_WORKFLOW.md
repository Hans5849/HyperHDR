# Agent workflow: cadence, work, review, release and shutdown

Policy revision: `AP-2026.11.21-7` · Standard: consolidated edition 1.0 · Authority: [AGENT_POLICY.md](AGENT_POLICY.md) · Adapter: [AGENT_ADAPTER.md](AGENT_ADAPTER.md) · PM Control: issue #74 · Operations log: issue #75

The primary measure is **accepted criterion weight against an approved baseline**. A closed issue, a merged PR, a successful scheduler run or a clean scan does not by itself establish acceptance.

**Section references from revision `-4`.** Prompts written under `-4` may cite old section numbers:

| Old reference | Now |
|---|---|
| Coordinator run (old §10) | §3 |
| Work orders (old §4) | §4 |
| Execution loop (old §6) | §8 |
| PR gates (old §7) | §10 |
| Validation matrix (old §11) | §13 |
| Sprint 1 shutdown and receipts (old §12) | §14 |

## 1. Cadence (owner amendment)

The edition's four-hour rhythm is replaced by a **16-hour coordination cycle** (owner, 2026-11-20). Everywhere the edition says "four-hour", read "16-hour". That covers recovery sweeps and staleness.

The cycle reconciles work; it does not require code each time. "No ready work" is a valid no-op.

**Planning.** Plan in one-week outcome increments, or two weeks for larger integrations. Priority bands are P0 (critical defects and release blockers), P1 (increment-critical), P2 (normal backlog) and P3 (proposals).

## 2. Roles and sessions

| Role | Session (persistent) | Notes |
|---|---|---|
| A | `session_01EsaroDQqk4fBzeUceenoCJ` | The only automated publisher of the #74 body. |
| B1 | `session_01Esaro8ss3ax9YJBavmmnn5` | Writer 1. Schedules disabled. |
| B2 | See the setup receipt on #74 | Writer 2. Schedules disabled. |
| D | `session_01Esaro9zPz325qLhtXNxvDV` | Correctness review. Distinct from B and F. |
| F | `session_01EsaroBCzJDi3ScMeW3YJ8Z` | Security review. Distinct from B and D. |
| Shutdown control | `session_01EsaroCsc36j7qPLVSRPa4N` | Dedicated; outside A–F. |

**Session continuity.** A role may continue its session or start a new run. Neither retained chat history nor a successful scheduler status proves usable repository access, so every run re-verifies its checkout, credentials and durable records before writing.

**Durable records.** Instructions, approvals, orders, claims and evidence live on GitHub and must survive replacement of a session.

**Prompt changes.** On this platform, a task's prompt can be changed only from the session it posts into ([AGENT_ADAPTER.md](AGENT_ADAPTER.md)).

## 3. Schedule

Every cron is prefixed `CRON_TZ=America/New_York`. Cron cannot express a 16-hour interval directly, so the cycle repeats every 48 hours on day-of-month parity.

| Role | Odd days (New York) | Even days (New York) | Crons |
|---|---|---|---|
| A | 00:10, 16:10 | 08:10 | `10 0,16 1-31/2 * *` · `10 8 2-30/2 * *` |
| B1 (A + 45 min) | 00:55, 16:55 | 08:55 | `55 0,16 1-31/2 * *` · `55 8 2-30/2 * *` |
| B2 (B1 + 3 min) | 00:58, 16:58 | 08:58 | `58 0,16 1-31/2 * *` · `58 8 2-30/2 * *` |
| D recovery sweep | 01:15, 17:15 | 09:15 | `15 1,17 1-31/2 * *` · `15 9 2-30/2 * *` |
| F recovery sweep | 01:45, 17:45 | 09:45 | `45 1,17 1-31/2 * *` · `45 9 2-30/2 * *` |

**D and F also run on demand** when B calls them (§9). The sweeps recover missed requests; they are not the only review opportunity.

**Time zone and daylight saving.** This is a wall-clock alternative to the edition's default of UTC elapsed time, so its DST behaviour is stated here:
- Sprint 1 (2026-11-20 to 2026-12-12) is entirely in EST (UTC−05:00). DST ended on 2026-11-01 and next begins on 2027-03-14.
- A sprint that spans a transition must restate and test this schedule.
- Gaps are 16 hours, except one 8-hour gap after the 31st of a month (and after 29 February).

**Records.**
- Record intended and actual times, the UTC offset, and a unique cycle key (`coord-`, `dev1-`, `dev2-`, `rev-`, `sec-` plus the scheduled UTC time as `YYYYMMDDTHHMMZ`).
- Deduplicate repeated triggers.
- A missed run does not extend a work order and never implies a handoff.
- Mark the dashboard **stale** after a missed 16-hour refresh. Recorded acceptance stands, but current health shows as Unverified.

**A coordinator run.**
1. Reconcile live issues, PRs, checks, approvals and previous runs.
2. Verify accepted progress.
3. Publish the #74 body and the attention queue.
4. Deliver any missing notices.
5. Check claims, resource use and review age.
6. Rank the approved queue, and publish bounded orders and follow-on orders within capacity.
7. Dispatch or recover blocking reviews within the standing limits.
8. Post a cycle report (§15).

## 4. Work orders

Orders are posted on the operations log (#75) and linked from #74.

| Field group | Required fields |
|---|---|
| Identity and outcome | Order ID; run ID; repository; release and component; approved issue and criterion IDs; objective; acceptance; scope and non-goals |
| Candidate and authority | Target branch; base SHA; policy revision; approval links; dependencies; allowed and prohibited actions; risk tier; declared paths |
| Execution and review | Worker (B1 or B2) and profile (Standard or Detailed); D and F assignment and applicability; installation-profile revision; test IDs; fallback; ranked follow-on orders |
| Limits and reporting | Runtime, CI, review and retry budgets; created time; claim and execution deadlines; campaign cutoff; lease identity; report destination |

**READY.** A publishes READY only when authority, prerequisites and capacity are all valid.
- The claim deadline is after the worker's planned start plus the allowed scheduling delay.
- B rereads live policy, approvals, branch state and dependencies before claiming.
- A missing, expired, superseded or inconsistent order produces a recorded no-op or blocker.

**Lifecycle.** DRAFT → READY → CLAIMED → RUNNING → REPORTED → RECONCILED, with the outcomes BLOCKED, FAILED, EXPIRED and CANCELLED.
- A verifies the actual artifacts before reconciling. Scheduler success alone is not enough.
- Claim expiry does not prove the executor stopped.

**Follow-on orders.** A may publish an active order plus ranked, already-authorized, independent follow-ons for each worker. Each issue keeps its own order, claim and evidence trail.

## 5. Decisions, notices and risks

**Decision records.** Every consequential decision and owner action lives in, or is linked from, #74, with four sections:

| Section | Contents |
|---|---|
| Context | ID, type and revision; author and time; repository; release or component; the exact question; reason, scope and evidence; owner and due date |
| Options | Alternatives and tradeoffs; recommendation; deferral impact; the exact action proposed; room for a custom owner response |
| Comments and decision | Dated questions, conditions and rationale; decision permalink; approving owner; exact scope, candidate or revision; validity and revocation |
| Execution | Action owner; next step and due date; status; verification evidence |

- Decision states: Awaiting, Approved, Rejected, Deferred or Superseded.
- Execution states: Not started, In progress, Blocked, Verified or Cancelled.
- Superseded decisions are preserved next to their replacements.

**Issue notices.** These follow the create-and-notify rule:
- Check for duplicates first, then open the issue and post a same-run notice on #74. The notice records: issue URL, author and time, creating run, reason, release or component, owner, next action, and whether a decision is needed.
- The options offered for a notice are: acknowledge or backlog, reprioritize, mark as a duplicate (with the canonical link), or close with a rationale.
- Acknowledgment authorizes nothing.

**Notice delivery.**
- The creating agent owns delivery and keeps an outbox entry: repository and issue, #74 URL, confirmed notice permalink, last attempt, failure reason and retry owner.
- Delivery is tracked as Pending, Posted or Failed, separately from acknowledgment. Posted means delivery was verified.
- A reconciles new issues against notices every cycle.

**Baseline changes and risks.** These are recorded in `PROJECT_PLAN.md`:
- **Baseline change:** reason, impacts, old and new baseline, owner decision and effective revision.
- **Risk:** cause → event → consequence; likelihood and impact; owner; trigger; mitigation; contingency; review date.

## 6. Parallel writers and the waiting cap

**Writers.**
- Two writers, B1 and B2, each with one active assignment and its own order, run ID, branch, writable worktree and lease. D and F are read-only and use no writer slot.
- A assigns parallel work only when the declared paths, interfaces and dependencies are compatible.
- These each have one owner at a time: lockfiles, CMake toolchain files, CI workflows, `CHANGELOG.md`, test registry structure and shared schemas.
- A worker that needs undeclared paths stops that change and asks for reassignment.

**Waiting cap.** At most **three distinct candidates** may be waiting on D, a required F review, or the owner, per repository. Each PR counts once, and #74 shows separate stage counts and the oldest ages. At the cap, prioritize review responses, CI repair, and approved work that adds no waiting candidate.

## 7. Leases and recoverable claims

**Required lease record.**
- **Identity:** repository, workstream, holder, order and run IDs, and an ownership **generation**.
- **Lifetime:** acquired time, heartbeat, expiry, execution deadline and campaign cutoff.
- **Control:** protected resources, permitted mutations, checkpoint and conditional release.

**What a valid lease mechanism must do.**
- Acquisition, renewal and release are atomic and conditional.
- Results and shared mutations are accepted only under the current generation, so a resumed old worker cannot overwrite its successor.
- CLAIM and RELEASE comments on #75 are the audit trail. Comments and labels are not locks.

**Status: no verified lease mechanism exists.**
- Creating a `claim/<issue>` ref can pick an initial winner, but it does not provide renewal, safe release or exclusion of a stale worker.
- GitHub `updateRefs` with expected-OID gives conditional ref changes, not a complete lease system.
- Until a mechanism is verified (D-003), **no unattended takeover of an expired claim**: confirm the previous executor stopped first.

**Ownership and recovery.**
- Checkpoint before yielding ownership.
- Reviewers get read-only assignments, not writer leases.
- Reacquire the writer lease before repairing a reviewed candidate.

## 8. Development cycle and model routing

**Before changing anything,** B reads both entrypoints, #74, #75, the applicable approvals and its order. It then verifies the repository, checkout, credentials, dependencies, policy revision, base and lease.

| Step | Required result |
|---|---|
| Implement | A focused `claude/<topic>` branch and PR tied to the approved criteria, with the dependency order declared |
| Prove | Registered tests and local results. For a defect: show the failure on unfixed code or a controlled mutant in a disposable worktree, then show the pass |
| Review | Independent D and applicable F requests (§9), with the exact candidate identity |
| Report | Current SHA, outcomes, limitations, remaining acceptance, next action and lease disposition |

**Integrity.** Never weaken assertions, disable coverage or relabel a failure.

**Follow-on work.** After reporting and releasing, B may claim the next eligible follow-on within its limits, without waiting for another cycle.

**Model routing.**
- Profiles: **Standard** for routine GREEN or AMBER work, tests, docs and CI repair; **Detailed** for architecture, concurrency, subtle data or security work, RED investigation and release-candidate review.
- Detailed or RED work gets Detailed correctness review, and F uses Detailed for every release candidate.
- The owner-approved matrix is in [AGENT_ADAPTER.md](AGENT_ADAPTER.md) §3 (D-008). Record the actual configuration for each run.
- Escalate after two unsuccessful Standard attempts, or on a D or F request.
- Never silently downgrade a profile. If Detailed capacity is exhausted, queue or escalate.

## 9. Blocking reviews

**When B may call D or F.** When a missing or stale review blocks an authorized candidate, B may trigger D for correctness and F for a required security review. This is standing dispatch authority, within the recorded concurrency, usage, retry and campaign limits (D-008). It needs no per-call PM decision once development is active.

**Limits.** B cannot approve its own work, waive a gate, expand scope, raise the review generation, or swap reviewers to get a different answer.

**Records.**
- **Request:** request ID; PR; criteria; D or F; head and base SHAs; policy and profile revisions; tests; known findings; question; reviewer; budget; deadline; result destination.
- **Receipt:** request ID; reviewer session; independence; reviewed candidate and scope; checks; findings; disposition; limitations; permalink; completion time; current or stale status.

**Deduplication.** The idempotency key is `repository + PR + role + full head SHA + policy revision + profile revision + reviewed base`. Reuse an active request or an applicable completed record.

**Rejected and changed candidates.**
- An unchanged rejected candidate stays rejected.
- A changed candidate needs renewed records. A delta review may carry earlier evidence forward if it states exactly what carries forward.

**Retries and waiting.**
- Transient-dispatch retries and repair rounds have separate caps, with bounded backoff.
- B watches only for its configured interval, then checkpoints and releases its writer lease.
- Every review execution is registered for shutdown.

**Adapter status.** On-demand dispatch is through `send_message` to the D or F session. It is **unverified**: the role sessions declined relayed configuration requests, and review dispatch hasn't been tested (AGENT_ADAPTER.md §2). Until it is verified, only the recovery sweeps apply.

## 10. Correctness review and merge gates

**D's review.** D reads the requirements, criteria and constraints and the full current diff, including tests, workflows, documentation and the changelog. It verifies behaviour, failure paths, compatibility and whether the tests prove the claim, then records **PASS** or **BLOCKED** for the named candidate and criteria.

**D and F are separate gates.** D does not substitute for F or for owner acceptance.

**Gates, re-checked immediately before any merge or handoff.** They are posted as a gate table on the PR.

| Gate | Evidence |
|---|---|
| Authority | A current delegation (none today) with its scope, exclusions and validity, and no stricter prohibition |
| Correctness and security | A current D disposition and F record for the exact head (the public fork requires F on every PR). Findings and exceptions explicitly resolved |
| Validation | Exact-head CI; current integration-candidate checks; local and platform suites; evidence of test integrity |
| Dependencies | Predecessor order; correct target; compatible integration; permitted merge method |
| Race protection | Live head, base, policy and approval re-checked; the validated candidate protected; the merge result recorded |

Pending, missing, cancelled, skipped-required or old-candidate results are not passes. With no delegation, prepare the PR for the owner to merge.

**Integration.** Use a merge queue with `merge_group` checks if one is verified. Otherwise integrate one PR at a time under the branch rules.

**Base advances.** A base advance needs current integration validation, not automatic rebases. An expected-head comparison alone does not guard against a concurrent base advance; if the validated candidate cannot be protected, unattended merge is deferred.

## 11. Security

Security is governed by [SECURITY_PLAN.md](SECURITY_PLAN.md) and [INSTALLATION_PROFILE.md](INSTALLATION_PROFILE.md). The public-fork profile means F reviews every PR and every final release candidate.

**F dispositions.** PASS, BLOCKED or NOT APPLICABLE, plus owner exceptions. A missing or stale F record is never a pass.

## 12. Integration, acceptance and release

**Integration preview.** C, or B under an order, builds an isolated preview of the approved PRs in dependency order and records the input SHAs, the preview commit and its tree. It runs the combined suite and a disposable installation rehearsal.

**Final candidate.**
- Merge each PR only after its §10 gates pass.
- Compare the final release tree with the preview.
- Rerun the full suite and the rehearsal on the actual final SHA.
- Obtain F's final-candidate disposition against the current profile.

**Acceptance batches.**

| Batch | What it covers |
|---|---|
| **R** (rehearsal) | A clean, disposable environment only |
| **I** (operational) | Explicit owner approval for the named target and profile. Covers backups, then the install, upgrade, interruption-recovery, rollback and uninstall cases. On the Orange Pi, the operator performs the physical steps (`docs/rk3588-acceptance-run.md`) |

**Acceptance evidence.**
- Each acceptance case records:
  - criterion and test IDs, tester, time, machine, versions and the exact SHA;
  - commands, the expected and observed results, and evidence;
  - deviations, stop conditions and rollback.
- Missing hardware or access means the case is Blocked.
- Hosted CI never establishes operational acceptance.
- Use `Refs #N` (not a closing reference) while host acceptance is pending.

**Release package.** E, or A when E is omitted, assembles:
- the release and baseline revisions, accepted criteria, the final SHA and artifact digests;
- D and F dispositions, CI and platform results, and operational acceptance;
- open defects, risks and exceptions;
- the changelog, rollback, and the exact release action requested.

An unmet mandatory gate means a no-go recommendation. The owner authorizes publication, and the deadline never grants it.

**After publication.** Tags are immutable, and published tags are never moved.

## 13. Setup states and activation checks

Setup begins in **CONFIGURING**. Under setup authority, agents draft missing records and provision control records. That does not enable development or grant merge, production, release or protected-policy authority.

**Overall results.** READY, PARTIALLY CONFIGURED or BLOCKED, recorded on #74 as SETUP RECEIPT comments and addenda.

**Role activation.**
- Each role acts only if the latest receipt's `Active roles:` line lists it. This applies once the role's prompt carries that guard; `-4`-era prompts require `Overall result: Ready`, which stops them.
- B1 and B2 schedules stay **disabled** until READY and D-003.

**Activation checks.** All must pass, or be covered by a current owner exception:

1. Repository and checkout identity, authenticated effective permissions, tools and dependencies.
2. Isolated writable worktrees for B1 and B2. Independent D and F identities. F has code-read and review/report access.
3. A **tested** restricted security reporting channel, and a named response owner.
4. Equivalent entrypoints at the same revision (`T-AGENT-ENTRYPOINTS` green on `master`). Evidence that the designated reviewers can retrieve.
5. A representative unattended initialization or resumption, and a recovery.
6. Issue-notice delivery and retry deduplication.
7. Test-registry execution.
8. On-demand D/F dispatch and retrieval of the result.
9. Drills:
   - duplicate trigger;
   - failed claim;
   - resumed expired worker (generation fencing);
   - changed candidate;
   - missing F record, stale SHA, stale profile, expired exception, failed scanner, restricted finding;
   - shutdown while a worker is busy.
10. Current-head and current-base merge enforcement. Disabling a trigger is distinct from stopping an execution.
11. Stored schedules and enabled states read back. Next runs are provider-reported, or labelled as calculated.

Declare READY only when the required records are adopted and the mandatory checks pass. Then activate the registered schedules and read them back.

## 14. Sprint 1 cutoff, shutdown and recovery

**Dates.** Sprint `hyperhdr-sprint-20261120`:
- Deadline: **2026-12-12 23:59 America/New_York**.
- Cutoff and shutdown: **23:14 (2026-12-13T04:14:00Z)**.
- Named human fallback: Hans5849, who also verifies the stopped state at the deadline.

**Inventory.** It lives in the latest SETUP RECEIPT and its addenda on #74, plus any INVENTORY ADDITION comment. Every scheduled task, recovery sweep, extra worker, on-demand review, retry and spawned subworker is added as it appears. Untracked automation and unilateral deadline extensions are prohibited.

**Shutdown sequence.**
1. **Stop.** At the cutoff, set the campaign to STOPPING on #75. Stop orders, claims and review dispatch. Disable every campaign trigger and read each back.
2. **Checkpoint.** Cancel queued executions and notify active holders. Checkpoint or safely stop every worker and review, preserving branches, SHAs, logs and findings. Disabling a trigger does not stop an execution.
3. **Release.** Verify cessation, or enforce the current generation fence, before any conditional lease release. A worker that cannot be stopped stays an explicit blocker.
4. **Hand off.** Post the shutdown receipt and final handoff on #74, and refresh it after late checkpoints. The handoff covers:
   - control state;
   - ownership and evidence;
   - outstanding work, including unreviewed candidates and redacted security findings;
   - exceptions with their expiry;
   - owner follow-ups.

**After the deadline.** A restart needs owner direction, a refreshed receipt and a newly verified shutdown control.

**Runtime failure before the cutoff.**
- Reconcile the actual branches, claims, checks and notices before retrying.
- Stop the affected workstream on an authentication failure, a stale order, an exhausted budget or a repeated validation failure.

## 15. Reports and measures

**Cycle report** (every A run):
- run and order IDs; base → current SHA;
- delivery and test evidence; claims and run health; accepted weight gained;
- blockers, with owners and unblock conditions;
- usage; notices and failures;
- the next three authorized actions, or the no-op reason.

**Daily report** (the first A run each UTC day): completion by project, release and component; baseline movement; implemented-but-unaccepted work; flow and oldest waits; resource use; PM attention items.

**Weekly / increment report:** remaining capabilities; forecast range and confidence; risks and debt; review capacity; accepted value; lessons; recommended decisions.

**Measures:**
- accepted delivery;
- flow and PM capacity (D, F, CI and PM wait ages);
- quality (reopened acceptance, regressions, rework);
- automation and cost (missed, duplicate and no-op runs; model and CI use).

Unknown values stay Unknown.

## 16. Evidence record templates

- **Cycle result:**
  - run and order IDs, role, profile, model and times;
  - objective; candidate (base → head, with the PR);
  - result: delivered work versus acceptance, and the accepted weight change;
  - validation; blockers; handoff.
- **Acceptance case:**
  - the criterion or test ID and the tester;
  - target SHA, machine and versions;
  - procedure, expected result, stop condition and rollback;
  - observed result (Pass, Fail or Blocked) with evidence;
  - disposition.
- **Bounded merge delegation:**
  - authority: owner permalink, repository and branch;
  - scope: issue classes, exclusions, protected paths;
  - gates: D, F, tests and integration;
  - method, with freshness enforcement;
  - validity: start, expiry and revocation;
  - execution: the actual PR, head and result.
