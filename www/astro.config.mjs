// @ts-check
import { defineConfig } from "astro/config";
import mdx from "@astrojs/mdx";
import sitemap from "@astrojs/sitemap";
import { unified } from "@astrojs/markdown-remark";
import { remarkDocsLinks } from "./src/scripts/docs-links.ts";

// https://astro.build/config
export default defineConfig({
    site: "https://umutsevdi.github.io/imza",
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
        build: {
            terserOptions: {
                compress: {
                    drop_console: true,
                    drop_debugger: true
                },
                mangle: true
            }
        }
    }
});
