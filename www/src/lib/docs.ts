import fs from "node:fs";
import path from "node:path";

// The wiki clone (materialized by scripts/ensure-wiki.mjs) is the single
// source of truth: pages, order (NN_ prefix), and titles (first h1) are read
// from it at build time, never hand-maintained. Resolved from the process cwd
// because the prerender bundle's import.meta.url points into dist/.
const WIKI_DIR = path.resolve(process.cwd(), "src/content/wiki");

export interface DocPage {
  file: string;
  slug: string;
  title: string;
}

function readWikiPages(): DocPage[] {
  const files = fs
    .readdirSync(WIKI_DIR)
    .filter((f) => f.endsWith(".md"))
    .sort((a, b) => {
      // numbered pages in filename order, then Home last for the pager
      const na = Number.parseInt(a, 10) || Number.MAX_SAFE_INTEGER;
      const nb = Number.parseInt(b, 10) || Number.MAX_SAFE_INTEGER;
      return na === nb ? a.localeCompare(b) : na - nb;
    });
  return files.map((file) => {
    const base = file.replace(/\.md$/, "");
    const numbered = base.match(/^(\d+)_(.*)$/);
    const stem = numbered ? numbered[2] : base;
    const slug = base === "Home" ? "" : stem.toLowerCase().replace(/_/g, "-");
    const h1 = fs
      .readFileSync(path.join(WIKI_DIR, file), "utf8")
      .match(/^#\s+(.+)$/m);
    const title = h1 ? h1[1].trim() : stem.replace(/_/g, " ");
    return { file, slug, title };
  });
}

const PAGES: DocPage[] = readWikiPages();

export function docPages(): DocPage[] {
  return PAGES;
}

const BASE = import.meta.env.BASE_URL.replace(/\/$/, "");

/** Prefix a site-absolute path with the configured base (GitHub Pages subpath). */
export function withBase(path: string): string {
  return BASE + path;
}

export function hrefFor(slug: string): string {
  return withBase(slug === "" ? "/docs/" : `/docs/${slug}`);
}

export function docNeighbors(slug: string): { prev?: DocPage; next?: DocPage } {
  const idx = PAGES.findIndex((p) => p.slug === slug);
  if (idx < 0) return {};
  return { prev: PAGES[idx - 1], next: PAGES[idx + 1] };
}
