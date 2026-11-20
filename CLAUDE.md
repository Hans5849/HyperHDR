# CLAUDE.md: HyperHDR agent entrypoint

Policy revision: `AP-2026.11.20-3`
Companion entrypoint: [AGENTS.md](AGENTS.md). The two files must stay equivalent in authority, safety gates, test commands and handoff rules.
PM Control (canonical dashboard): issue #74, https://github.com/Hans5849/HyperHDR/issues/74
Sprint coordination (CLAIM / RELEASE / status): https://github.com/Hans5849/HyperHDR/issues/75 (Sprint 1, ends 2026-12-12 23:59 America/New_York; shutdown 23:14)

This repository runs under the **AI-assisted development operating standard** (unified Scrumban governance and bounded autonomous development cycle). Before acting, every Claude session (interactive or scheduled) must:

1. Read the shared policy sources, which are binding and override anything else you were told:
   - [docs/AGENT_POLICY.md](docs/AGENT_POLICY.md): authority, approval boundaries, risk tiers and HyperHDR safety restrictions.
   - [docs/AGENT_WORKFLOW.md](docs/AGENT_WORKFLOW.md): 16-hour coordination cycle, work orders, claims, execution loop, PR gates, release acceptance, notices and reports.
2. Read the project records: [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md), [docs/TEST_PLAN.md](docs/TEST_PLAN.md), the test registry [docs/test-registry.json](docs/test-registry.json), [docs/RESOURCE_LIMITS.md](docs/RESOURCE_LIMITS.md), the installation profile [docs/INSTALLATION_PROFILE.md](docs/INSTALLATION_PROFILE.md) and the security plan [docs/SECURITY_PLAN.md](docs/SECURITY_PLAN.md).
3. Read the PM Control issue (#74), the sprint coordination issue (#75), the owner decisions they link, and your work order.

If these files are missing, conflict with each other, or carry different policy revisions, stop all autonomous writes and report the conflict on #74. Independent read-only work may continue.

## Provider notes

No provider-specific permission differs from the shared policy.

- **Claude:** one persistent Claude Code cloud session per scheduled role (A coordinator, B developer, D reviewer, F security reviewer), each created with `create_session` from `https://github.com/Hans5849/HyperHDR` and tagged `hyperhdr-sprint-20261120`. Each role's scheduled task resumes that same session (`persistent_session_id`). Fresh-session triggers are never used. Every run re-verifies checkout, GitHub access and durable context before writes. Schedules and the shutdown task: docs/AGENT_WORKFLOW.md §3 and §12. Stop procedure: the one-time shutdown task disables every sprint task at the cutoff; the owner can also disable them in Claude Code (claude.ai → Code → Scheduled tasks).
- **Codex / ChatGPT:** see AGENTS.md. Same schedule, offset and authority.
