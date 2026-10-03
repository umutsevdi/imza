import { createMarkdownProcessor } from "@astrojs/markdown-remark";

// Mirrors the markdown section of astro.config.mjs minus remarkDocsLinks
// (wiki-relative links only) so standalone files render like collection entries.
const processor = await createMarkdownProcessor({
  shikiConfig: { theme: "github-dark-default" }
});

export async function renderMarkdown(source: string): Promise<string> {
  const { code } = await processor.render(source);
  return code;
}
