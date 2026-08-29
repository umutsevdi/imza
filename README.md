# ursa

Ursa is a native C++23 coding agent with an ftxui terminal interface. It
streams markdown responses, calls tools, edits the current workspace, and
supports OpenAI-compatible and Anthropic-compatible APIs.

## What works today

- Streaming chat with markdown, reasoning, usage, and cost display
- Multiple provider connections with runtime model and reasoning selection
- OpenAI Chat Completions and Anthropic Messages wire formats
- Plan mode with read-only tools and build mode with the complete toolset
- `read`, `list`, `shell`, `edit`, `write`, `ask`, and `todo` tools
- Approval flows, structured question forms, diff views, and tool errors
- Queued prompts, interruption, rate-limit retries, and countdowns
- Automatic context compaction before the active model reaches its context
  limit, with visible progress in the transcript
- Automatic session saving and a recent-first `/sessions` picker for loading
  and deleting saved sessions
- Project instructions from `AGENTS.md`, `CLAUDE.md`, or `GEMINI.md`
- Workspace and repository status, changed files, branch, and todo widgets
- Background generation of a concise session title from the first prompt

## Priority roadmap

The next capabilities are ordered by dependency and user value:

1. **Skills** — discover and load reusable instruction packages with explicit
   scope and invocation rules.
2. **MCP** — expose external MCP tools through the existing flat
   `ToolRegistry`.
3. **Subagents** — run bounded parallel tasks, show live status in the sidebar,
   and keep durable results in chat.
4. **LSP** — add definitions, references, symbols, hover, and diagnostics after
   the core session and extension systems are stable.

## Build and test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTESTS=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target ursa ursa_tests
./build/debug/ursa_tests
```

Dependencies are fetched by CMake: ftxui, jsoncpp, cmark-gfm, and doctest.
OpenSSL and CURL must be available on the system.

Run the application with:

```sh
./build/debug/ursa
```

On first launch, use `/connect` to add a provider and `/model` to select a
model. `/help` lists all commands.

## Sessions

Ursa automatically saves non-empty sessions when exiting, including exits via
Ctrl+C and Ctrl+D. Before loading another session, the current session is also
saved. Saved state includes the title, transcript, attachments, reasoning,
tool calls and results, diffs, todos, active mode, compacted context, and the
workspace directory. Loading a session changes back to its saved workspace
before restoring the transcript; loading fails if that directory is no longer
available.

Run `/sessions` to open the session picker. Sessions are sorted newest first:

- `↑` and `↓` move the cursor
- `Enter` loads the selected session
- `d` asks for confirmation before deleting the selected session
- `Esc` closes the picker or cancels deletion

Loading is disabled while the model is connecting, thinking, or streaming,
while a tool is unresolved, or while user messages are queued. Finish or
interrupt the pending work before loading another session. Deleting saved
sessions remains available while work is pending.

Session files are timestamped JSON files stored in the platform data directory:

- Linux: `$XDG_DATA_HOME/ursa`, falling back to
  `$HOME/.local/share/ursa`
- macOS: `$HOME/Library/Application Support/ursa`
- Windows: `%APPDATA%\ursa`

## Context compaction

When reported prompt usage reaches 80% of the active model's known context
limit, Ursa summarizes older model-facing history while preserving the current
turn verbatim. The complete visible transcript is retained. During compaction,
the transcript shows `Compacting…`; after the summary is committed it changes
to `✓ Session compacted`. Providers or models without a known context limit do
not trigger automatic compaction.

## Configuration

Ursa stores `ursa/config.json` in the platform configuration directory:

- Linux: `$XDG_CONFIG_HOME/ursa/config.json`, falling back to
  `$HOME/.config/ursa/config.json`
- macOS: `$HOME/Library/Application Support/ursa/config.json`
- Windows: `%APPDATA%\ursa\config.json`

The UI normally writes this file. A minimal custom OpenAI-compatible connection
looks like this:

```json
{
  "providers": [
    {
      "id": "openai",
      "provider_id": "custom",
      "endpoint": "https://api.openai.com/v1/chat/completions",
      "api_key": "sk-..."
    }
  ],
  "last_used": {
    "provider": "openai",
    "model": "gpt-4.1"
  },
  "reasoning_effort": "off"
}
```

Provider catalog metadata is cached beside the config as `presets.json`.
Credentials are currently stored as plain JSON; protect the config file with
normal user-only filesystem permissions.

## Documentation

- [Documentation index](docs/README.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Developer guide](docs/DEVELOPER-GUIDE.md)
- [Provider connections](docs/connect.md)
- [Modal system](docs/modal-system.md)
- [Agent and tool design](docs/coding-agent-design.md)
- [Known limitations](docs/CODEBASE-REVIEW.md)

Project rules and coding conventions live in [AGENTS.md](AGENTS.md).
