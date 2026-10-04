import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineCollection, z } from "astro:content";
import { glob } from "astro/loaders";

// The imza.wiki clone is materialized here by scripts/ensure-wiki.mjs.
const wikiDir = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "content/wiki"
);

const docs = defineCollection({
  loader: glob({
    pattern: "*.md",
    base: wikiDir,
    // keep the exact filename (minus .md) as id: "Home", "01_Installation",
    // so lib/docs.ts slugFor() can derive route slugs from it.
    generateId: ({ entry }) => entry.replace(/\.md$/, "")
  }),
  schema: z.object({
    title: z.string().optional()
  })
});

// CHANGELOG.txt is mirrored to src/content/changelog.md by
// scripts/ensure-wiki.mjs; scope a collection to that single file.
const changelog = defineCollection({
  loader: glob({
    pattern: "changelog.md",
    base: path.resolve(path.dirname(fileURLToPath(import.meta.url)), "content"),
    generateId: () => "changelog"
  }),
  schema: z.object({})
});

export const collections = { docs, changelog };
