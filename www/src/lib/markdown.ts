import { createMarkdownProcessor } from "@astrojs/markdown-remark";

// Mirrors the markdown section of astro.config.mjs (minus remarkDocsLinks,
// which only applies to wiki-relative links) so standalone files render
// exactly like collection entries.
const processor = await createMarkdownProcessor({
  shikiConfig: { theme: "github-dark-default" }
});

export async function renderMarkdown(source: string): Promise<string> {
  const { code } = await processor.render(source);
  return code;
}
