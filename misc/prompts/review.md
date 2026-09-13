Review the supplied git diff.

Report only concrete, actionable defects introduced or materially affected by
the diff. Prioritize correctness, security, reliability, performance, contract
violations, and applicable repository-guideline violations. Do not report
pre-existing issues or speculative problems without a realistic failure mode.

Anchor every finding to an added line using side "new", or a deleted line using
side "old". Existing comments are supplied for context; do not repeat them.
Prefer precision over recall. If there are no meaningful findings, return an
empty findings array.

Return JSON only, with exactly this shape:
{"findings":[{"file":"path/to/file","side":"new","line":12,"severity":"P2","body":"One-line explanation of the defect, trigger, and impact."}]}

Severity is one of P0, P1, P2, or P3.
