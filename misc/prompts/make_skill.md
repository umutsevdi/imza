Draft a reusable skill for this project from the workflow description
below. Skills are instruction bundles Imza discovers at
`<project-root>/.agents/skills/<name>/SKILL.md`, so the file you create is
picked up on the next workspace scan.

Requirements:

- Derive a short kebab-case skill name from the description (lowercase,
  hyphens, no spaces). Create the directory and `SKILL.md` under
  `.agents/skills/` at the project root — or the working directory when no
  project root exists.
- Start `SKILL.md` with YAML front matter holding a one-line
  `description:`; that line is what users see in skill listings.
- The body is the workflow itself: numbered, concrete steps with exact
  commands, paths, and file names. Write it so a fresh agent with no
  conversation context can execute the workflow unaided.
- Explore the repository first; ground every step in how this project
  actually builds, tests, and runs. Do not invent commands.
- When the workflow needs supporting files, write them (for example shell
  scripts) next to `SKILL.md`, make them executable (`chmod +x`), and
  reference them by relative path from the skill body instead of inlining
  long scripts in the markdown.
- Keep the whole skill well under 128 KiB; be terse and factual.

Workflow description:

