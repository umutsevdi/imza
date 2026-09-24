# Imza

**Imza is a lightweight coding agent designed to improve the development 
experience for both models and the people using them.**

It keeps you engaged with the codebase by surfacing diffs throughout 
development and making review a first-class part of the workflow.

For the agent, Imza provides a sandboxed programming environment where reads, 
edits, searches, and commands can be composed into larger operations. This 
reduces unnecessary tool calls and context overhead while keeping permissions 
enforced and agent actions observable.

Better orchestration can also reduce inference costs by avoiding unnecessary 
model round trips and intermediate context[^1].

> Imza is a native C++ application. It is not lightweight because it does less.

**\~20 MB binary · \~5-8 MB RAM at startup · \~20–40 MB during typical agentic work[^2]**

https://github.com/user-attachments/assets/a0096f0d-8337-4e6b-aabe-f9debe273594

Download the [latest release](https://github.com/umutsevdi/imza/releases/latest).

Check out the [user guide](https://github.com/umutsevdi/imza/wiki).

## Why Imza?

Existing coding agents makes the development faster, but causes the developer gradually stops 
reading the code.

Plans, summaries, tool output, and completion messages start replacing direct 
interaction with the implementation. The agent keeps working, while your 
understanding of how the codebase is actually changing can fall behind.

Imza is designed to keep the developer inside that implementation loop.

Development is separated into three explicit modes:

- **Plan**: Investigate the repository, understand the problem, and design the 
change without modifying files. The agent can explore freely while the codebase 
remains read-only.

- **Build**: Implement the plan using edits, commands, subagents, and 
programmatic tool execution. Imza surfaces code changes as they happen, while 
filesystem access and shell commands remain subject to explicit permissions.

- **Review**: Inspect the resulting Git diff, add findings manually or with 
the agent's help, and send those findings directly back into another planning 
pass.

This makes review part of the development loop rather than only a final 
checkpoint:

**Plan → Build → Review → Plan → Build**

Imza aims to give the agent autonomy over the mechanical work without removing 
the developer from the code itself. You still see what is being written, review 
the resulting implementation, and decide how the next iteration should proceed.

![imza-layout](./screenshots/layout.png)

## Highlights

* Plan → Build → Review workflow with review findings fed back into planning
* Interactive diffs and AI-assisted code review throughout development
* Sandboxed tool orchestration for fewer round trips and less context overhead
* Shell-aware, scoped permission controls
* Sidechat for asking questions without disrupting the main agent's work
* Up to five concurrent research or build subagents
* Bring-your-own-model support, including local OpenAI-compatible models
* Native terminal UI with a small runtime footprint
* Persistent sessions, transcripts, and automatic context compaction
* Headless execution for scripts, CI, and development tooling
* Terminal notifications when an attended agent finishes or needs input

## Bring Your Own Model

Imza supports OpenAI-compatible endpoints and the Anthropic Messages API, 
including locally hosted OpenAI-compatible models.

Use your own API credentials, a subscription-backed connection where supported, 
or a model running locally.

## Headless Mode

Run Imza non-interactively from scripts, CI jobs, or other development tools.

Use `--ask` for a one-shot, read-only query or `--exec` for a one-shot 
task that can modify files.

Grant only the directories and commands required by the task:

```sh
imza --exec "build the project and summarize the changes" \
  --allow-dir ../shared-assets \
  --allow-cmd "git status" "cmake --build"
```

`--allow-dir` grants access beneath the specified directory.

A quoted `--allow-cmd` value such as `"git status"` grants only that 
command/subcommand pair. Providing only the program name grants access to all 
of its subcommands for the current session.

## Composable Tool Execution

Instead of requiring a separate model round trip for every read, search, edit, 
or command, Imza exposes its tools through a sandboxed programming environment.

The model can compose multiple operations into a single execution, making tasks 
such as filtering files, gathering context, and performing coordinated edits 
more efficient.

These operations still go through Imza's permission and mutation tracking 
systems, so shell execution, filesystem access, and resulting code changes 
remain observable.

In Imza's own development sessions, this approach substantially reduced 
intermediate tool output and context overhead. [Read the analysis](https://github.com/umutsevdi/imza/discussions/1).

## Capabilities

### Workflow and Review

* [x] Plan, Build, and Review modes
* [x] Interactive diffs
* [x] Generated and manual review comments
* [x] Review → Plan handoff
* [x] Structured questions and task tracking
* [x] Prompt queueing and generation interruption
* [x] Sidechat

### Agent Execution

* [x] File reading, editing, and shell execution
* [x] Sandboxed composable tool execution
* [x] Concurrent subagents
* [x] Persistent subagent transcripts
* [x] Configurable subagents
* [x] Web search and page fetching
* [x] Skills and project instructions
* [x] `@path` file attachments
* [x] `$skill` attachments
* [X] Image and multimodal prompt attachments

### Permissions and Control

* [x] Tool approval flows
* [x] Scoped filesystem and skill permissions
* [x] Shell-aware program and subcommand grants

### Models and Context

* [x] OpenAI-compatible APIs
* [x] Anthropic Messages API
* [x] Local OpenAI-compatible servers
* [x] Streaming Markdown and reasoning
* [x] Automatic context compaction
* [x] Repository, context, token, and cost usage information

### Sessions and Interface

* [x] Persistent local sessions
* [x] Syntax highlighting
* [x] Headless mode
* [x] Terminal notifications
* [X] Image and multimodal prompt attachments

## Roadmap

* [ ] Extension System
* [ ] MCP support
* [ ] Background watchdogs that notify the agent when files or processes reach a target state
* [ ] Local monthly usage analytics

## Installation

Pre-built packages are available from the [latest release](https://github.com/umutsevdi/imza/releases/latest).

> Downloaded macOS packages are currently unsigned. On first launch, 
> right-click the application and select Open to allow it through Gatekeeper.

After installation, run `imza`.

On first launch, use `/connect` to configure a provider and `/model` to select a model.

Type `/` in the chat input to browse the available commands.

> Installed builds check for new releases and notify you when one is available. 
> Run `imza --update` to install the latest version.

### Building from Source

Want to build Imza yourself or contribute to development?

See the [build and installation guide](https://github.com/umutsevdi/imza/wiki/01_Installation#building-from-source)
for dependencies, CMake configuration, packaging, and platform-specific instructions.

---

[^1]: <https://github.com/umutsevdi/imza/discussions/1>
[^2]: Memory use may increase with conversation history, loaded skills, and
Tree-sitter languages used in Review mode.
