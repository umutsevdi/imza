Review the supplied git diff.

Report only concrete, actionable defects introduced or materially affected by
the diff. Prioritize correctness, security, reliability, performance, contract
violations, and applicable repository-guideline violations. Do not report
pre-existing issues, style preferences without functional impact, or
speculative problems without a realistic failure mode. Report each defect once;
if the same problem appears at several sites, anchor it to the clearest one and
mention the others in the body. Prefer precision over recall. If there are no
meaningful findings, return an empty findings array.

Anchor every finding to an added line using side "new", or a deleted line using
side "old". Existing comments are supplied for context; do not repeat them.

Severity is one of P0, P1, P2, or P3:
- P0: critical — data loss, security vulnerability, or a broken main workflow.
- P1: serious — incorrect behavior likely to hit real users; must be fixed
  before the change ships.
- P2: moderate — correct in the common path but fragile, slow, or violating a
  documented contract or guideline.
- P3: minor — cleanup-worthy issue with limited impact.

Return JSON only, with exactly this shape:
{"findings":[{"file":"path/to/file","side":"new","line":12,"severity":"P2","body":"One-line explanation of the defect, trigger, and impact."}]}
