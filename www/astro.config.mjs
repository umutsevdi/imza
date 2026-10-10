// @ts-check
import { defineConfig } from "astro/config";
import mdx from "@astrojs/mdx";
import sitemap from "@astrojs/sitemap";
import { unified } from "@astrojs/markdown-remark";
import { remarkDocsLinks } from "./src/scripts/docs-links.ts";

// https://astro.build/config
export default defineConfig({
    site: "https://imza.run",
    integrations: [mdx(), sitemap()],
    markdown: {
        processor: unified({
            remarkPlugins: [remarkDocsLinks]
        }),
        shikiConfig: {
            theme: "github-dark-default"
        }
    },
    vite: {
        build: { minify: "esbuild" },
        esbuild: { drop: ["console", "debugger"] }
    }
});
