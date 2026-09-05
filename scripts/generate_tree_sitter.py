#!/usr/bin/env python3

"""Generate Ursa's Tree-sitter build sources and syntax registry.

To add a language:
1. Add its grammar at vendor/tree-sitter/<package>:
   git submodule add --depth 1 <repository> vendor/tree-sitter/<package>
   To use a release other than the repository's current default, check it out
   in the submodule before committing. The parent repository's gitlink pins the
   selected commit, and git submodule add updates .gitmodules automatically.
2. Add one entry to LANGUAGES below. The key is the canonical Markdown fence
   name. Usually only extensions is required.
3. Set package when its directory differs from the language name, grammar_path
   when parser.c is not under src/, symbol for a nonstandard exported function,
   highlight_bases for inherited queries, and filenames for extensionless or
   specially named files such as Dockerfile and CMakeLists.txt.
4. Reconfigure CMake. It runs this generator and validates the grammar sources,
   highlight queries, file types, and filenames.

The defaults are package=<name>, grammar_path=".", symbol=tree_sitter_<name>,
and highlight query vendor/tree-sitter/<package>/queries/highlights.scm.
"""

from __future__ import annotations

import argparse
import hashlib
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Language:
    extensions: tuple[str, ...]
    filenames: tuple[str, ...] = ()
    package: str | None = None
    grammar_path: str = "."
    symbol: str | None = None
    highlight_bases: tuple[str, ...] = ()


LANGUAGES: dict[str, Language] = {
    "cpp": Language(
        extensions=("cpp", "cc", "cxx", "c", "hpp", "hh", "hxx", "h"),
        highlight_bases=("c",),
    ),
    "html": Language(extensions=("html", "htm")),
    "css": Language(extensions=("css",)),
    "javascript": Language(
        extensions=("js", "mjs", "cjs", "jsx"),
    ),
    "typescript": Language(
        extensions=("ts", "mts", "cts"),
        grammar_path="typescript",
        highlight_bases=("javascript",),
    ),
    "go": Language(extensions=("go",)),
    "rust": Language(extensions=("rs",)),
    "swift": Language(extensions=("swift",)),
    "dockerfile": Language(
        extensions=(),
        filenames=("dockerfile", "containerfile"),
    ),
    "java": Language(extensions=("java",)),
    "cmake": Language(
        extensions=("cmake",),
        filenames=("cmakelists.txt",),
    ),
    "dart": Language(extensions=("dart",)),
    "make": Language(
        extensions=("mk", "mak"),
        filenames=("makefile", "gnumakefile", "bsdmakefile"),
    ),
    "json": Language(extensions=("json",)),
    "lua": Language(extensions=("lua",)),
    "python": Language(extensions=("py", "pyw", "pyi")),
    "php": Language(
        extensions=("php", "phtml"),
        grammar_path="php",
    ),
    "bash": Language(
        extensions=("sh", "bash"),
        filenames=(".bashrc", ".bash_profile", ".profile"),
    ),
    "powershell": Language(extensions=("ps1", "psm1", "psd1")),
}


@dataclass(frozen=True)
class ResolvedLanguage:
    name: str
    definition: Language
    symbol: str
    sources: tuple[Path, ...]
    query: str


def _package_name(name: str, language: Language) -> str:
    return language.package or name


def _package_root(root: Path, package: str) -> Path:
    path = root / package
    if not path.is_dir():
        raise ValueError(
            f"Tree-sitter package '{package}' is missing at {path}. "
            "Run: git submodule update --init --recursive"
        )
    return path


def _resolve_sources(root: Path, name: str, language: Language) -> tuple[Path, ...]:
    package = _package_root(root, _package_name(name, language))
    source_directory = package / language.grammar_path / "src"
    parser = source_directory / "parser.c"
    if not parser.is_file():
        raise ValueError(f"Tree-sitter parser is missing: {parser}")

    scanners = tuple(
        path
        for path in (
            source_directory / "scanner.c",
            source_directory / "scanner.cc",
            source_directory / "scanner.cpp",
        )
        if path.is_file()
    )
    if len(scanners) > 1:
        paths = ", ".join(str(path) for path in scanners)
        raise ValueError(f"Multiple Tree-sitter scanners found for '{name}': {paths}")
    return (parser, *scanners)


def _resolve_query(root: Path, name: str, language: Language) -> str:
    packages = (*language.highlight_bases, _package_name(name, language))
    parts: list[str] = []
    for package in packages:
        query = _package_root(root, package) / "queries" / "highlights.scm"
        if not query.is_file():
            raise ValueError(f"Tree-sitter highlight query is missing: {query}")
        parts.append(query.read_text(encoding="utf-8"))
    return "\n".join(parts)


def _validate_catalog(root: Path) -> None:
    owners: dict[str, str] = {}
    filename_owners: dict[str, str] = {}
    for name, language in LANGUAGES.items():
        if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
            raise ValueError(f"Invalid language name: {name}")
        for file_type in (name, *language.extensions):
            normalized = file_type.casefold()
            if normalized.startswith("."):
                normalized = normalized[1:]
            owner = owners.setdefault(normalized, name)
            if owner != name:
                raise ValueError(
                    f"File type '{normalized}' is owned by both '{owner}' and '{name}'"
                )
        for filename in language.filenames:
            normalized = filename.casefold()
            owner = filename_owners.setdefault(normalized, name)
            if owner != name:
                raise ValueError(
                    f"Filename '{normalized}' is owned by both '{owner}' and '{name}'"
                )
        _package_root(root, _package_name(name, language))
        for package in language.highlight_bases:
            _package_root(root, package)


def _resolve_languages(root: Path) -> tuple[ResolvedLanguage, ...]:
    _validate_catalog(root)
    return tuple(
        ResolvedLanguage(
            name=name,
            definition=language,
            symbol=language.symbol or f"tree_sitter_{name}",
            sources=_resolve_sources(root, name, language),
            query=_resolve_query(root, name, language),
        )
        for name, language in LANGUAGES.items()
    )


def _cpp_string(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


def _raw_cpp_string(value: str) -> str:
    digest = hashlib.sha256(value.encode("utf-8")).hexdigest()[:8]
    delimiter = f"ursa_{digest}"
    if f"){delimiter}\"" in value:
        raise ValueError("Unable to construct a safe raw C++ string delimiter")
    return f'R"{delimiter}({value}){delimiter}"'


def _render_string_array(name: str, values: tuple[str, ...]) -> str:
    identifier = name.upper()
    if not values:
        return f"constexpr std::array<std::string_view, 0> {identifier} {{ }};"
    entries = ", ".join(
        f"std::string_view {{ {_cpp_string(value)} }}" for value in values
    )
    return f"constexpr std::array {identifier} {{ {entries} }};"


def _render_registry(languages: tuple[ResolvedLanguage, ...]) -> str:
    declarations = "\n".join(
        f'extern "C" const TSLanguage* {language.symbol}();'
        for language in languages
    )
    definitions: list[str] = []
    entries: list[str] = []
    for language in languages:
        identifier = language.name.upper()
        definitions.extend(
            (
                _render_string_array(
                    f"{identifier}_EXTENSIONS", language.definition.extensions
                ),
                _render_string_array(
                    f"{identifier}_FILENAMES", language.definition.filenames
                ),
                f"constexpr std::string_view {identifier}_HIGHLIGHTS = "
                f"{_raw_cpp_string(language.query)};",
            )
        )
        entries.append(
            "    LanguageSpec { "
            f'{_cpp_string(language.name)}, {identifier}_EXTENSIONS, '
            f"{identifier}_FILENAMES, {language.symbol}, "
            f"{identifier}_HIGHLIGHTS }},"
        )

    return (
        f"{declarations}\n\n"
        + "\n\n".join(definitions)
        + f"\n\nconstexpr std::array<LanguageSpec, {len(languages)}> LANGUAGE_SPECS {{ {{\n"
        + "\n".join(entries)
        + "\n} };\n"
    )


def _cmake_path(path: Path) -> str:
    return str(path.resolve()).replace("\\", "/").replace('"', '\\"')


def _render_sources(languages: tuple[ResolvedLanguage, ...]) -> str:
    sources = "\n".join(
        f'    "{_cmake_path(source)}"'
        for language in languages
        for source in language.sources
    )
    return f"set(URSA_TREE_SITTER_GRAMMAR_SOURCES\n{sources}\n)\n"


def _write_if_changed(path: Path, content: str) -> None:
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    core_cmake = root / "core" / "CMakeLists.txt"
    if not core_cmake.is_file():
        parser.error(
            "Tree-sitter runtime is missing. "
            "Run: git submodule update --init --recursive"
        )
    try:
        languages = _resolve_languages(root)
    except ValueError as error:
        parser.error(str(error))
    _write_if_changed(
        args.output / "tree_sitter_sources.cmake", _render_sources(languages)
    )
    _write_if_changed(
        args.output / "syntax_registry.inc", _render_registry(languages)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
