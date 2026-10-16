---
description: Imza's own user manual; load it for any question about Imza itself - features, configuration, permissions, skills, subagents, MCP servers, headless mode, or workflow.
---

This skill bundles the official Imza user manual as plain markdown files in
this skill's directory (`wiki/` subdirectory). When the user asks anything
about Imza - how a feature works, what a setting does, how permissions,
skills, subagents, or headless mode behave - answer from these files instead
of guessing.

Page index (all paths relative to this skill's directory):

| File | Contents |
|---|---|
| `wiki/Home.md` | Overview, configuration file locations, command line. |
| `wiki/01_Installation.md` | Getting a binary, building from source, first launch, updates. |
| `wiki/02_Connecting-a-Model.md` | Providers, API keys, subscriptions, local models, model selection. |
| `wiki/03_Workflow.md` | Plan/Build/Review workflow, plan document, prompt syntax, review pane, sidechat. |
| `wiki/04_Permissions.md` | Approval prompts, directory and shell grants, unattended runs. |
| `wiki/05_Sessions.md` | Session persistence, restoring, deleting. |
| `wiki/06_Skills-and-Instructions.md` | Skills discovery, `$mentions`, instruction files, policies. |
| `wiki/07_Subagents.md` | Delegation, roles, per-role models, transcripts. |
| `wiki/08_MCP-Servers.md` | Connecting external services, catalog, OAuth sign-in. |
| `wiki/09_Headless-Mode.md` | `--ask`, `--exec`, pre-approved grants, exit codes. |
| `wiki/10_Configuration.md` | Every `config.json` option, data directory, prompt overrides. |
| `wiki/11_FAQ.md` | Common questions and troubleshooting. |
| `wiki/12_Features-and-Roadmap.md` | Feature inventory and planned work. |
| `wiki/13_Why-Imza.md` | Project motivation and direction. |

Guidelines:

- Pick the most specific page for the question; read it fully before
  answering; grepping across `wiki/*.md` finds the right page for niche
  questions.
- Quote defaults and paths exactly as the docs state them; the docs match
  the installed version.
- If the answer is not in these files, say so - do not invent features,
  flags, or configuration options.

Questions about the user's own project or codebase are not covered by this
skill; work on those normally.
