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
remains read-only, and the outcome is a plan document: a Markdown spec with a 
required skeleton
that lives beside the conversation instead of getting lost in it. Every 
revision is kept. You can pin notes to any part of the document and send them 
back to the agent as one revise request, iterating until the plan says exactly 
what you intend.

- **Build**: Agent implements the plan using edits, commands, subagents, and 
programmatic tool execution. The approved plan stays with the agent as the 
reference for the work. 

Imza displays changes as they happen, keeping the developer informed.

- **Review**: Inspect the resulting Git diff, add findings manually or with 
the agent's help, and send those findings back into another planning pass as a 
new revision of the plan.

This makes review part of the development loop rather than only a final 
checkpoint:

**Plan → Build → Review → Plan → Build**

Imza aims to give the agent autonomy over the mechanical work without removing 
the developer from the code itself. You still see what is being written, review 
the resulting implementation, and decide how the next iteration should proceed.

![imza-layout](./screenshots/layout.png)

## Highlights

* Plan → Build → Review workflow built around a co-editable plan document, 
with review findings fed back into planning as revisions
* Interactive diffs and AI-assisted code review throughout development
* Sandboxed tool orchestration for fewer round trips and less context overhead
* Shell-aware, scoped permission controls
* Sidechat for asking questions without disrupting the main agent's work
* Bring-your-own-model support, including local OpenAI-compatible models
* Native terminal UI with a small runtime footprint
* Up to five concurrent research or build subagents
* Persistent sessions, transcripts, and automatic context compaction
* `@path` file and `$skill` skill attachments, plus image and multimodal prompts
* Session-level token, context, and cost usage insight
* Headless execution for scripts, CI, and development tooling
* Terminal notifications when an attended agent finishes or needs input

[Full feature list and roadmap](https://github.com/umutsevdi/imza/wiki/10_Features-and-Roadmap)

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
