import fs from "node:fs";
import path from "node:path";
import type { Root } from "mdast";
import { visit } from "unist-util-visit";

// imza.wiki pages link with bare filenames (04_Sessions, Home) and image
// paths (images/x.png); rewrite them to site routes before rendering.
const WIKI_DIR = path.resolve(process.cwd(), "../wiki");

function pageSlugs(): Record<string, string> {
  const map: Record<string, string> = {};
  for (const f of fs.readdirSync(WIKI_DIR)) {
    if (!f.endsWith(".md")) continue;
    const base = f.replace(/\.md$/, "");
    if (base === "Home") {
      map[base] = "";
      continue;
    }
    const m = base.match(/^(\d+)_(.*)$/);
    if (m) {
      map[base] = m[2].toLowerCase().replace(/_/g, "-");
    }
  }
  return map;
}

const PAGE_SLUGS = pageSlugs();

export function remarkDocsLinks() {
  return (tree: Root) => {
    visit(tree, "link", (node) => {
      const url = node.url;
      if (url.startsWith("http") || url.startsWith("#")) {
        return;
      }
      const hashIdx = url.indexOf("#");
      const target = hashIdx >= 0 ? url.slice(0, hashIdx) : url;
      const hash = hashIdx >= 0 ? url.slice(hashIdx + 1) : undefined;
      const slug = PAGE_SLUGS[target];
      if (slug !== undefined) {
        node.url = slug === "" ? "/docs/" : `/docs/${slug}`;
        if (hash) {
          node.url += `#${hash}`;
        }
      }
    });
    visit(tree, "image", (node) => {
      if (node.url.startsWith("images/")) {
        node.url = "/docs-images/" + node.url.slice("images/".length);
      }
    });
  };
}
