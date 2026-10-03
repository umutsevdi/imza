// @ts-check
import eslint from "@eslint/js";
import tseslint from "typescript-eslint";

const astro = await import("astro-eslint-parser");
// eslint's LooseParserModule type rejects the parser's richer return
// type; the runtime shape is compatible.
const astroParser = /** @type {any} */ (astro);

export default tseslint.config(
  { ignores: ["dist", ".astro", "node_modules"] },
  eslint.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ["scripts/**/*.mjs"],
    languageOptions: {
      globals: {
        console: "readonly",
        process: "readonly"
      }
    }
  },
  {
    files: ["**/*.astro"],
    languageOptions: {
      parser: astroParser,
      parserOptions: {
        parser: "@typescript-eslint/parser",
        extraFileExtensions: [".astro"]
      },
      globals: {
        process: "readonly",
        fetch: "readonly",
        AbortSignal: "readonly"
      }
    },
    rules: {
      "@typescript-eslint/no-unused-vars": [
        "error",
        { argsIgnorePattern: "^_" }
      ]
    }
  }
);
