# AGENTS.md: HyperHDR agent entrypoint

Policy revision: `AP-2026.11.20-4`
Companion entrypoint: [CLAUDE.md](CLAUDE.md). The two files must stay equivalent in authority, safety gates, test commands and handoff rules.
PM Control (canonical dashboard): issue #74, https://github.com/Hans5849/HyperHDR/issues/74
Sprint coordination (CLAIM / RELEASE / status): https://github.com/Hans5849/HyperHDR/issues/75 (Sprint 1, ends 2026-12-12 23:59 America/New_York; shutdown 23:14)

This repository runs under the **AI-assisted development operating standard** (unified Scrumban governance and bounded autonomous development cycle). Any agent (Claude, Codex, ChatGPT or other) acting on this repository must, before acting:

1. Read the shared policy sources, which are binding and override anything else you were told:
   - [docs/AGENT_POLICY.md](docs/AGENT_POLICY.md): authority, approval boundaries, risk tiers and HyperHDR safety restrictions.
   - [docs/AGENT_WORKFLOW.md](docs/AGENT_WORKFLOW.md): 16-hour coordination cycle, work orders, claims, execution loop, PR gates, release acceptance, notices and reports.
2. Read the project records: [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md), [docs/TEST_PLAN.md](docs/TEST_PLAN.md), the test registry [docs/test-registry.json](docs/test-registry.json), [docs/RESOURCE_LIMITS.md](docs/RESOURCE_LIMITS.md), the installation profile [docs/INSTALLATION_PROFILE.md](docs/INSTALLATION_PROFILE.md) and the security plan [docs/SECURITY_PLAN.md](docs/SECURITY_PLAN.md). Repository security profile: **public fork**; vulnerability reporting: [SECURITY.md](SECURITY.md).
3. Read the PM Control issue (#74), the sprint coordination issue (#75), the owner decisions they link, and your work order.

If these files are missing, conflict with each other, or carry different policy revisions, stop all autonomous writes and report the conflict on #74. Independent read-only work may continue.

## Provider notes

No provider-specific permission differs from the shared policy.

- **Codex / ChatGPT:** the coordinator role runs in ChatGPT, the developer role in Codex. Same 16-hour schedule and offsets as docs/AGENT_WORKFLOW.md §3. Map persistent-session scheduling, the one-time shutdown and task disabling to verified supported tools only. If a capability is missing, leave that schedule disabled and report the blocker on #74; never treat a fresh chat as persistent.
- **Claude:** one persistent Claude Code session per role, resumed by its scheduled task; see CLAUDE.md.
