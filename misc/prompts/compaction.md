Summarize this coding-agent session for continuation. Preserve the user's
requirements, decisions, files changed, commands and test results, unresolved
problems, and the exact current task. Be concise and do not continue the task.
Summarization should have three sections.

Preserve all information necessary to continue execution, in rough priority
order — the earlier items matter most:

* The user's actual goal and the exact current request and task.
* Current implementation state.
* Important requirements, constraints, preferences, and acceptance criteria.
* Decisions already made and why they matter.
* Any exact strings, identifiers, values, URLs, versions, or snippets that
  would be difficult or risky to reconstruct.
* Files created, modified, inspected, or still relevant, including important
  paths.
* Important functions, classes, APIs, schemas, commands, configuration, and
  architectural details.
* Tool calls or shell commands whose results affect future work.
* Errors encountered, failed approaches, and what was learned from them.
* Tests/checks already performed and their results.
* Open questions, unresolved issues, TODOs, and the next concrete actions.

Discard:

* Conversational filler.
* Repeated explanations.
* Superseded plans or hypotheses, unless knowing they failed prevents repeated
  work.
* Low-level reasoning that does not affect future actions.
* Tool output that can safely be summarized without losing operationally
  important details.

Prefer dense factual statements over prose. Do not optimize for readability at
the expense of losing state.

Use this structure:

## Objective

What the user ultimately wants.

## Current State

What has been completed and the state of the implementation/work.

## Important Decisions

Decisions that future work should preserve, including brief rationale where
necessary.

## Relevant Files / Artifacts

Paths and what matters about each one.

## Remaining Work

Specific unfinished tasks.

## Next Action

The exact next thing the continuing agent should do.
