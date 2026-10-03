import { existsSync, cpSync, mkdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const repoRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../.."
);
const wiki = path.join(repoRoot, "wiki");

if (existsSync(path.join(wiki, "Home.md"))) {
  copyImages(wiki);
  process.exit(0);
}

if (existsSync(wiki)) {
  console.error(
    `wiki/ exists at ${wiki} but has no Home.md — ` +
      "not cloning over it. Fix or remove the directory."
  );
  process.exit(1);
}

console.log("wiki/ missing — cloning umutsevdi/imza.wiki…");
execFileSync(
  "git",
  ["clone", "https://github.com/umutsevdi/imza.wiki.git", wiki],
  { stdio: "inherit" }
);
copyImages(wiki);

// Serve wiki images as-is under /docs-images/ for the /docs pages.
function copyImages(wikiDir) {
  const src = path.join(wikiDir, "images");
  const dest = path.join(repoRoot, "www", "public", "docs-images");
  if (!existsSync(src)) {
    return;
  }
  mkdirSync(dest, { recursive: true });
  cpSync(src, dest, { recursive: true });
}
