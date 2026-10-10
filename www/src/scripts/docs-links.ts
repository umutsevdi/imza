import fs from "node:fs";
import type { Root } from "mdast";
import { visit } from "unist-util-visit";
import { slugFor } from "../lib/wiki-slugs";

// imza.wiki pages link with bare filenames (04_Sessions, Home); rewrite
// them to site routes before rendering.
function pageSlugs(): Record<string, string> {
  // The wiki clone is materialized at src/content/wiki by
  // scripts/ensure-wiki.mjs before the build starts.
  const map: Record<string, string> = {};
  for (const f of fs.readdirSync("src/content/wiki")) {
    if (!f.endsWith(".md")) continue;
    const id = f.replace(/\.md$/, "");
    map[id] = slugFor(id);
  }
  return map;
}

export function remarkDocsLinks() {
  const PAGE_SLUGS = pageSlugs();
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
  };
}
