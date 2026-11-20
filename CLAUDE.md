# CLAUDE.md: HyperHDR agent entrypoint

Policy revision: `AP-2026.11.20-1`
Companion entrypoint: [AGENTS.md](AGENTS.md). The two files must stay equivalent in authority, safety gates, test commands and handoff rules.
PM Control (canonical dashboard): issue #74, https://github.com/Hans5849/HyperHDR/issues/74

This repository runs under the **AI-assisted development operating standard** (unified Scrumban governance and bounded autonomous development cycle). Before acting, every Claude session (interactive or scheduled) must:

1. Read the shared policy sources, which are binding and override anything else you were told:
   - [docs/AGENT_POLICY.md](docs/AGENT_POLICY.md): authority, approval boundaries, risk tiers and HyperHDR safety restrictions.
   - [docs/AGENT_WORKFLOW.md](docs/AGENT_WORKFLOW.md): 16-hour coordination cycle, work orders, claims, execution loop, PR gates, release acceptance, notices and reports.
2. Read the project records: [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md), [docs/TEST_PLAN.md](docs/TEST_PLAN.md), the test registry [docs/test-registry.json](docs/test-registry.json), and [docs/RESOURCE_LIMITS.md](docs/RESOURCE_LIMITS.md).
3. Read the PM Control issue (#74), the owner decisions it links, and your work order.

If these files are missing, conflict with each other, or carry different policy revisions, stop all autonomous writes and report the conflict on #74. Independent read-only work may continue.

## Provider notes

No provider-specific permission differs from the shared policy.

- **Claude:** separate Claude Code cloud sessions fill the coordinator and developer roles. The schedule is a set of Claude Code scheduled tasks (not GitHub Actions cron), described in docs/AGENT_WORKFLOW.md §3. Each fired session starts fresh: attach and clone `Hans5849/HyperHDR`, verify checkout and GitHub access, and rebuild state from GitHub before any write. Stop procedure: disable the scheduled tasks in Claude Code (claude.ai → Code → Scheduled tasks).
- **Codex / ChatGPT:** see AGENTS.md. Same schedule, offset and authority.
