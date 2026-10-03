import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineCollection, z } from "astro:content";
import { glob } from "astro/loaders";

// The imza.wiki clone lives at the repo root, two levels up from www/.
const wikiDir = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../../wiki"
);

const docs = defineCollection({
  loader: glob({
    pattern: "*.md",
    base: wikiDir,
    // keep the exact filename (minus .md) as id: "Home", "01_Installation",
    // so lib/docs.ts page keys match.
    generateId: ({ entry }) => entry.replace(/\.md$/, "")
  }),
  schema: z.object({
    title: z.string().optional()
  })
});

export const collections = { docs };
