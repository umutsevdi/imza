// Single source of truth for wiki filename conventions. Shared by the
// content collection (content.config.ts), the remark link rewriter
// (runs outside Astro's module graph, so this file must stay
// dependency-free), and lib/docs.ts page helpers.

/** Route slug for a wiki entry id ("Home" -> "", "01_Installation" -> "installation"). */
export function slugFor(id: string): string {
  if (id === "Home") return "";
  const m = id.match(/^\d+_(.*)$/);
  const stem = m ? m[1] : id;
  return stem.toLowerCase().replace(/_/g, "-");
}
