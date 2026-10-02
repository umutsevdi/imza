# imza website

Static marketing site for the imza terminal coding agent, built with
[Astro](https://astro.build).

This is a self-contained npm project inside the imza repository. It does
not participate in the CMake build; the imza `clang-format`/`clang-tidy`
scripts do not touch it. Node.js >= 22.12 is required.

## Why the build looks odd

The site has almost no assets of its own — shared files live elsewhere
in the repository and are imported **in place**:

| Import                       | Actual source                                  |
| ---------------------------- | ---------------------------------------------- |
| screenshots (`plan/build/review`) | `../wiki/images/` — a clone of [imza.wiki](https://github.com/umutsevdi/imza.wiki) at the repo root |
| app icon                     | `../misc/icon.png` (`public/icon.png` is the favicon copy) |

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
- Images outside `src/` still go through `astro:assets` optimization
  (webp, sized, lazy-loaded).
- When a deploy pipeline is added, the same prebuild hook covers CI —
  no extra clone step needed in the workflow.

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
  layouts/       BaseLayout.astro — head, skip-link, global CSS, statusbar
  components/    One component per page section + SidePanel + Statusbar
  pages/         index.astro — composition only
  styles/        global.css — imza's ftxui palette and all section styles
```

Interactive behavior (hero type-in, Plan/Build/Review tab switching,
scroll-spy) lives in co-located `<script>` blocks inside the component
that owns it — there are no separate script files.
