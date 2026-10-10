import { getCollection } from "astro:content";
import { slugFor } from "./wiki-slugs";

// Page helpers over the wiki content collection. Filename conventions
// live in lib/wiki-slugs.ts (kept dependency-free so the remark link
// rewriter can share them).

export interface DocPage {
  /** Collection entry id: "Home", "01_Installation", ... */
  id: string;
  /** Route slug: "" for Home, "installation" for 01_Installation, ... */
  slug: string;
  /** First h1 of the page body, falling back to the id stem. */
  title: string;
}

export async function docPages(): Promise<DocPage[]> {
  const entries = await getCollection("docs");
  return entries
    .map((e) => ({
      id: e.id,
      slug: slugFor(e.id),
      title:
        e.data.title ??
        h1Of(e.body) ??
        e.id.replace(/^\d+_/, "").replace(/_/g, " ")
    }))
    .sort((a, b) => {
      // numbered pages in id order, then Home last for the pager
      const na = Number.parseInt(a.id, 10) || Number.MAX_SAFE_INTEGER;
      const nb = Number.parseInt(b.id, 10) || Number.MAX_SAFE_INTEGER;
      return na === nb ? a.id.localeCompare(b.id) : na - nb;
    });
}

/** First level-1 heading of the markdown body, if any. */
function h1Of(body?: string): string | undefined {
  const m = body?.match(/^#\s+(.+)$/m);
  return m ? m[1].trim() : undefined;
}

export function hrefFor(slug: string): string {
  return slug === "" ? "/docs/" : `/docs/${slug}`;
}

export async function docNeighbors(slug: string): Promise<{
  prev?: DocPage;
  next?: DocPage;
}> {
  const pages = await docPages();
  const idx = pages.findIndex((p) => p.slug === slug);
  if (idx < 0) return {};
  return { prev: pages[idx - 1], next: pages[idx + 1] };
}
