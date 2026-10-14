Write the handoff document for this work: what was planned, and how it was implemented.

You are given the latest plan document, every earlier plan revision, the user's messages, the build log (the decisions the agent recorded while implementing), and a stat of the files that changed (path, change kind, and line counts).

Write the document directly in markdown, structured like a handoff a human engineer would leave for the next person:

- Open with a short summary: what this change is and why it exists.
- A section on what was planned: the intent from the plan, condensed — not copied.
- A section on how it was implemented: walk through the work by area or file, explaining what was actually built and the reasoning behind notable choices. Where the implementation diverges from the plan, say so in place — what changed, why, and what the user asked for along the way that redirected the work.
- Close with anything unfinished, unverified, or explicitly deferred.

Write for a reviewer who has not seen the conversation. Be concrete: name files and areas, keep it readable prose rather than status lists. Reply with the document only — no preamble, no code fence around the whole document.