import { copyFileSync, existsSync, mkdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

// The wiki clone lives inside the Astro source tree so markdown image
// references (images/x.png) resolve through the content pipeline and get
// hashed, base-aware URLs on any deploy target.
const wwwRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  ".."
);
const contentDir = path.join(wwwRoot, "src", "content");
const wiki = path.join(contentDir, "wiki");

// CHANGELOG.txt is repo-root canon (the app reads it from there); mirror it
// as a markdown entry so the site renders it through the content pipeline.
// The directory holds no tracked files (wiki/ and changelog.md are
// gitignored), so it does not exist on a fresh checkout.
mkdirSync(contentDir, { recursive: true });
copyFileSync(
  path.join(wwwRoot, "..", "CHANGELOG.txt"),
  path.join(wwwRoot, "src", "content", "changelog.md")
);

if (existsSync(path.join(wiki, "Home.md"))) {
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
