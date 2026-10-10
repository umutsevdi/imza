# imza website

Static marketing site for the imza terminal coding agent, built with
[Astro](https://astro.build).

This is a self-contained npm project inside the imza repository. It does
not participate in the CMake build; the imza `clang-format`/`clang-tidy`
scripts do not touch it. Node.js >= 22.12 is required.

## Why the build looks odd

The site has almost no assets of its own — shared files live elsewhere
in the repository and are imported **in place**:

| Import                            | Actual source                                                                                                                     |
| --------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| screenshots (`plan/build/review`) | `../wiki/images/` — a clone of [imza.wiki](https://github.com/umutsevdi/imza.wiki) at the repo root                               |
| app icon                          | `../misc/icon.png` (`public/icon.png` is the favicon copy)                                                                        |
| version (`src/version.ts`)        | `IMZA_VERSION` environment variable set by the build pipeline (same name as the app's compile definition); falls back to `v0.0.0` |
| `/changelog/` page                | `../CHANGELOG.txt`, mirrored to `src/content/changelog.md` by the prebuild hook and rendered as markdown                          |
| `/license/` page                  | `../LICENSE` + `../misc/LICENSE.thirdparty.txt`, read directly (rendered literally, not markdown)                                 |

Consequences:

- **`wiki/` must exist at the repo root or the build fails.** It is
  fetched automatically: `npm run dev` and `npm run build` run
  `scripts/ensure-wiki.mjs` first, which clones `imza.wiki` when the
  directory is missing. CI gets this for free too.

  `wiki/` is in the root `.gitignore` because it is a separate
  repository — it is not a submodule and is never committed here. If
  you need it by hand:

  ```sh
  git clone https://github.com/umutsevdi/imza.wiki.git wiki
  ```

- Screenshot and icon updates made upstream flow into the site on the
  next rebuild; nothing is duplicated under `www/`.
- The repo-root `CHANGELOG.txt`, `LICENSE`, and
  `misc/LICENSE.thirdparty.txt` are imported in place at build time and
  served at `/changelog/` and `/license/`. The version shown across the
  site comes from the `IMZA_VERSION` environment variable the pipeline
  sets for the build.
- All wiki images are served as-is from `public/docs-images/`
  (copied by the prebuild hook). The landing page and the docs pages
  share those same files, so no asset exists twice in `dist/`.
- When a deploy pipeline is added, the same prebuild hook covers CI —
  no extra clone step needed in the workflow.
- The site is served from the apex domain `https://imza.run/` at the
  root path, so all internal links and assets are plain root-relative
  paths (`/docs/`, `/_astro/...`). `public/CNAME` pins the custom
  domain for the GitHub Pages deploy.

## Development

```sh
npm ci
npm run dev        # dev server at localhost:4321
npm run build      # production build to ./dist/
npm run preview    # preview the production build
npm run lint       # tsc --noEmit + eslint
npm run prettier   # format src/
```

## Structure

```
src/
  layouts/       BaseLayout.astro — head, skip-link, global CSS import
  components/    One component per page section + SidePanel + Statusbar;
                 each carries its own scoped <style> block
  pages/         index.astro — composition only
  styles/        global.css — ftxui palette, reset, app shell, and the
                 widget styles shared by both side panels
```

Styling convention: component-specific rules live in a scoped
`<style>` block inside the component that owns them; `global.css` holds
only design tokens, the reset, base typography, the `.app` shell, and
patterns shared across components (`.tui-panel`, `.section-label`, the
side-panel widget family). Docs article styles (which target rendered
markdown) stay in `styles/docs.css`.

Interactive behavior (hero type-in, Plan/Build/Review tab switching,
scroll-spy) lives in co-located `<script>` blocks inside the component
that owns it — there are no separate script files.

## Docs pages (/docs)

The wiki clone is rendered at `/docs/` (Home) and `/docs/<slug>` for the
ten numbered pages. The repository root `CHANGELOG.txt` is mirrored to
`src/content/changelog.md` by the prebuild hook and rendered at
`/changelog/`. At build time a content collection globs
`src/content/wiki/*.md`; the `remarkDocsLinks` plugin rewrites
wiki-style links (`04_Sessions`, `Home`) to site routes
(`/docs/sessions`, `/docs/`).

- `src/lib/wiki-slugs.ts` - the single source of truth for wiki
  filename conventions (numbered prefixes give the order, `NN_Name`
  maps to the `name` slug). Adding a page to the wiki repo is all it
  takes; the routes, sidebar TOC, and pager follow automatically
- `src/lib/docs.ts` - page helpers over the docs content collection
  (slug, title, ordering, pager neighbors)
- `src/layouts/DocsLayout.astro` - index chrome (WikiSidePanel +
  statusbar) around the article, pager, and edit-on-GitHub link
- Editing docs content happens in the imza.wiki repo; this site picks it
  up on the next build.

Note: the remark plugin means the Markdown pipeline is the classic
`unified()` one (`markdown.processor` in `astro.config.mjs`), not the
newer Satteri default; the shiki gruvbox themes apply to docs code
blocks as before.
