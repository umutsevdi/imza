# ursa

A minimal interactive CLI coding agent with a terminal UI (C++23, ftxui).
Chat with an LLM that streams markdown answers and calls tools to work on coding
tasks in your workspace.

## Capabilities

- [X] streaming chat with markdown rendering (headings, code blocks, tables)
- [X] tool calls with approval modals (accept / accept always / reject + reason)
- [X] read files — whole file or a 1-based line window
- [X] list directories (with sizes)
- [X] run shell commands with a timeout
- [X] todo-list tracking (side panel)
- [X] ask-the-user question forms (options, multi-select, free text)
- [X] plan mode (read-only tools) / build mode
- [X] queue messages while the agent streams; cancel before send
- [X] slash commands (`/help`, `/exit`, `/settings`, `/prompt`, `/demo`)
- [X] project instruction files (`AGENTS.md`, `CLAUDE.md`, `GEMINI.md`)
- [X] OS/shell/package-manager detection in the system prompt
- [X] token & cost tracking; rate-limit retry with countdown
- [ ] file modification tools (`write`, `edit`) — the model cannot change files
- [ ] search tools (pattern `glob`, `grep`) and `webfetch`
- [ ] provider/model switching at runtime; multiple provider profiles
- [ ] custom slash commands / skills
- [ ] subagents and MCP tool servers
- [ ] session resume / conversation persistence
- [ ] image input
- [ ] hooks, LSP, IDE integration

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTESTS=ON
cmake --build build --target ursa ursa_tests
```

Dependencies are fetched automatically (ftxui, jsoncpp, cmark-gfm, doctest);
system OpenSSL and CURL are required.

## Run

```sh
./build/debug/ursa
```

## Configuration

A JSON config at the platform config directory:

- Linux: `$XDG_CONFIG_HOME/ursa/config.json` (fallback `~/.config/ursa/config.json`)
- macOS: `~/Library/Application Support/ursa/config.json`
- Windows: `%APPDATA%\ursa\config.json`

```json
{
  "api_base": "https://api.openai.com/v1",
  "api_key": "sk-...",
  "model": "gpt-4o",
  "standard": "openai|anthropic (default=openai)"
}
```
