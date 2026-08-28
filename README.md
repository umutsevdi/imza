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
- Project instructions from `AGENTS.md`, `CLAUDE.md`, or `GEMINI.md`
- Workspace and repository status, changed files, branch, and todo widgets

## Priority roadmap

The next capabilities are ordered by dependency and user value:

1. **Session persistence** — save and restore transcripts, state, workspace,
   model selection, and usage metadata with crash-safe versioned storage.
2. **Session management** — create, open, search, archive, and delete saved
   sessions.
3. **Context management** — compact or summarize long histories before they
   exceed a model's context window.
4. **Session titles** — derive a local title from the first prompt, support
   manual rename, and consider background AI titles later.
5. **Skills** — discover and load reusable instruction packages with explicit
   scope and invocation rules.
6. **MCP** — expose external MCP tools through the existing flat
   `ToolRegistry`.
7. **Subagents** — run bounded parallel tasks, show live status in the sidebar,
   and keep durable results in chat.
8. **LSP** — add definitions, references, symbols, hover, and diagnostics after
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
