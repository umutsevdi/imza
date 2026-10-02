// @ts-check
import { defineConfig } from "astro/config";
import mdx from "@astrojs/mdx";
import sitemap from "@astrojs/sitemap";

// https://astro.build/config
export default defineConfig({
    site: "https://umutsevdi.github.io/imza",
    integrations: [mdx(), sitemap()],
    markdown: {
        shikiConfig: {
            themes: {
                light: "gruvbox-light-soft",
                dark: "gruvbox-dark-soft"
            }
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
