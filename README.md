# Ursa

Ursa is a lightweight coding agent that runs in your terminal. 

Bring your own model connection, open Ursa in a project, and describe the
outcome you want. Ursa reads the repository instructions, gathers context,
asks for decisions when needed, and works through the task with visible tool
calls and approval prompts.

## Why Ursa?

Ursa aims to keep the useful core of tools such as OpenCode, Claude Code, and
Codex in a small native application:

- A responsive terminal interface instead of a browser or Electron shell;
- An approximately 8–15 MB runtime memory footprint;
- Explicit Plan and Build modes;
- Visible reasoning, tool activity, diffs, token usage, and cost;
- Provider choice without tying the application to one model vendor;
- Durable local sessions that can be reopened later;
- Bounded parallel delegation without hiding the delegated agents' work.

It is intentionally narrower than those larger tools. The checklist below is
also the current project status, not a promise that every competing product
implements a feature in exactly the same way.

## Capabilities

- [x] Stream Markdown responses and reasoning in the terminal
- [x] Read and list files, run shell commands, and create or edit text files
- [x] Preview file changes as diffs
- [x] Require approval for mutating tools, with allow-once, allow-for-session,
      and reject flows
- [x] Separate read-oriented Plan mode from full Build mode
- [x] Ask structured single-choice, multiple-choice, and free-text questions
- [x] Maintain a visible task list for longer work
- [x] Delegate up to five concurrent research or build subagents
- [x] Show a separate, persistent transcript for every delegated agent
- [x] Configure different models and reasoning variants for subagent roles
- [x] Discover and load project or global skills
- [x] Read project instructions from `AGENTS.md`, `CLAUDE.md`, or `GEMINI.md`
- [x] Attach workspace text files to a prompt with `@path`
- [x] Attach skills with `$skill`
- [x] Queue prompts and interrupt active generation
- [x] Retry rate-limited requests with a visible countdown
- [x] Compact model context automatically while retaining the visible chat
- [x] Save, load, and delete local sessions
- [x] Display repository state, changed files, context, and usage in the UI
- [x] Connect to OpenAI-compatible and Anthropic Messages APIs
- [x] Connect to local OpenAI-compatible servers

Compared with the broader extension ecosystems around OpenCode, Claude Code,
and Codex, Ursa does not currently provide:

- [ ] MCP servers or tools
- [ ] LSP-powered definitions, references, hover, or diagnostics
- [ ] Image or other multimodal prompt attachments
- [ ] Built-in web search or browser automation
- [ ] IDE integrations or a graphical desktop client
- [ ] Remote agents, cloud workspaces, or hosted session synchronization

## How it works

Ursa starts in Plan mode, where the model can inspect the workspace and
prepare an approach without changing files. Switch to Build mode when you want
it to edit code or run commands. Potentially mutating actions are presented for
approval before execution.

For independent work, the main agent can delegate one to five tasks in
parallel. Research agents remain read-oriented. Build agents are available only
when the main agent is in Build mode. Ursa waits for the group, returns every
report to the main agent, and keeps each agent's full chat available from its
own transcript button. Delegated agents cannot recursively delegate or alter
the main task list.

Sessions are saved locally and include the conversation, tool calls and
results, diffs, attachments, task list, active mode, compacted context, and
workspace. When the active model has a known context limit, Ursa compacts older
model-facing history at 80% usage without removing it from the visible
transcript.

## Build

Ursa requires a C++23 compiler, CMake, OpenSSL, and CURL. CMake fetches ftxui,
jsoncpp, cmark-gfm, and doctest.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTESTS=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target ursa ursa_tests
./build/debug/ursa_tests
```

Start Ursa from the repository you want it to work in:

```sh
./build/debug/ursa
```

On first launch, open `/connect` to add a provider, then use `/model` to choose
a model. Type `/` in the chat input to browse the available commands.

## Commands

| Command | Purpose |
| --- | --- |
| `/new` | Save the current session and start another |
| `/connect` | Add and manage provider connections |
| `/model` | Select the active model |
| `/variant` | Select the reasoning effort |
| `/subagents` | Configure models for delegated roles |
| `/sessions` | Load or delete saved sessions |
| `/skills` | Manage discovered skills |
| `/prompt` | Inspect the generated system prompt |
| `/exit` | Save and quit |

## Local data

Ursa stores `ursa/config.json` in the platform configuration directory:

- Linux: `$XDG_CONFIG_HOME/ursa/config.json`, or
  `$HOME/.config/ursa/config.json`
- macOS: `$HOME/Library/Application Support/ursa/config.json`
- Windows: `%APPDATA%\ursa\config.json`

Saved sessions use the platform data directory:

- Linux: `$XDG_DATA_HOME/ursa`, or `$HOME/.local/share/ursa`
- macOS: `$HOME/Library/Application Support/ursa`
- Windows: `%APPDATA%\ursa`

Provider credentials are currently stored as plain JSON. Protect the config
file with user-only filesystem permissions.

## Project status

Ursa is under active development. 
