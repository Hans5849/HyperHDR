# Provider adapter record: Claude Code cloud

Policy revision: `AP-2026.11.21-6` · Adapter revision: `ADP-1` · Owner: Hans5849 · Verified: 2026-11-20 to 2026-11-21, by the setup session (`session_01EsaroCaQRu7aWfttihA7tB`), from tool responses and readbacks

Edition 1.0 §15 requires every provider capability to be mapped to an operation the tools **actually expose**, with evidence. Legacy names from the playbook are examples, not guaranteed commands. This record covers the one provider in use. No OpenAI or Codex adapter is configured.

## 1. Provider and product surface

| Item | Value |
|---|---|
| Provider | Anthropic, Claude Code cloud sessions (claude.ai) |
| Repository access | GitHub MCP tools and git through the session proxy. The identity is `Hans5849` with the **admin** role (F readback, 2026-11-20). There is no per-role GitHub identity |
| Environment | `env_01DTsiaRFU9XPxYrBgTUXyoy`: ephemeral containers with no production secrets and no route to the Orange Pi |
| Model | `claude-opus-5-5` for every session (session_context). Separate reasoning-effort settings have not been verified per task |

## 2. Capability mapping

| Capability (edition §15) | Operation used | Evidence | Status and limitations |
|---|---|---|---|
| Initialize or resume | `create_session` (source repository, title, tag) creates a persistent session. A task with `persistent_session_id` resumes it | The A, B1, D, F and shutdown sessions each completed a read-only check (2026-11-20). Checkout, fetch and issue reads all succeeded | **Verified.** Chat history is not trusted; every run re-reads GitHub |
| Schedule and inspect | `create_trigger` (cron with `CRON_TZ`, `run_once_at`), `get_trigger`, `list_triggers` | Readbacks show cron, enabled state and `next_run_at` for every task | **Verified**, with these limits: (a) a task can't be created disabled, so it is created and then disabled; (b) `list_triggers` truncates large pages, so filter by `enabled`; (c) the next runs are provider-reported |
| Change a task's prompt | `update_trigger` with `prompt` | Refused from the setup session: "a routine's instructions can be changed only from the conversation the routine posts into" | **Limitation.** Prompt changes must be made from the task's own session, and the role sessions correctly decline relayed requests. They need the owner to ask directly |
| Enable or disable | `update_trigger` with `enabled` | B1's tasks were disabled and read back; the old 4-hourly task was disabled with the owner's authority (2026-11-20) | **Verified** |
| Dispatch independent review | `send_message` to the D or F session | Delivery is confirmed (`status: delivered`). The sessions process messages as peer data and decline configuration changes | **Unverified for review requests.** A review request hasn't been tested. The recovery sweeps are the fallback |
| Stop and recover | `update_trigger enabled=false` with readback; `interrupt_session`; `list_sessions` with tag; `get_session` status | Disable and readback verified. Interrupt and cancel are **not tested** | **Partially verified.** Disabling a trigger doesn't stop a running session. The busy-session shutdown drill hasn't run |
| Control shared writes (leases) | None verified | — | **Not available.** No atomic generation-fenced lease. `git push` of a new ref is atomic for creation only. GraphQL `updateRefs` was not tested (AGENT_WORKFLOW §7) |
| Integrate and publish | GitHub MCP issue, comment and PR tools; no merge used | Issues #74–#79, comments and edits all succeeded | **Verified** for evidence and notices. Merging is out of scope (delegation NONE) |
| Security advisories | None | No tool creates or reads advisories | **Not available** (D-006 gap 1) |
| Alerts (code scanning, Dependabot, secret scanning) | None. `run_secret_scanning` scans supplied content only | F readback | **Not available** (D-006 gap 3) |
| Disable a workflow | None | No tool | **Not available.** It's an owner action in the Actions UI (SF-006 was done this way) |

## 3. Model routing matrix (proposed; needs owner approval, D-008)

| Role | Standard profile | Detailed profile | Budget per run (proposed) |
|---|---|---|---|
| A Coordinator | `claude-opus-5-5`, default effort | — (A does no Detailed work) | 1 session turn; no code |
| B1, B2 Developer | `claude-opus-5-5`, default effort | `claude-opus-5-5`, high effort | 1 order per run plus follow-ons until the next A slot; CI watch ≤ 45 min |
| D Correctness | `claude-opus-5-5`, default effort | `claude-opus-5-5`, high effort (required for Detailed or RED work) | 1 candidate per dispatch |
| F Security | `claude-opus-5-5`, high effort | `claude-opus-5-5`, high effort (required for release candidates) | 1 candidate per dispatch |

**Limits of this matrix.**
- Effort settings are proposals. The current tasks don't set effort explicitly (`model` is empty, so the session default applies).
- Using a different provider adds a perspective, but it never replaces a required role or its independence.
- Any new mapping needs owner approval and a verification of this adapter.
