#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

PROMPT_NAMES: tuple[str, ...] = (
    "system",
    "subagent",
    "subagent_research",
    "subagent_build",
    "title",
    "compaction",
    "make_skill",
    "review",
    "review_plan",
    "plan_annotations",
)


def _raw_cpp_string(value: str) -> str:
    delimiter = f"imza_{hashlib.sha256(value.encode('utf-8')).hexdigest()[:8]}"
    if f'){delimiter}\"' in value:
        raise ValueError("Unable to construct a safe raw C++ string delimiter")
    return f'R"{delimiter}({value}){delimiter}"'


def _normalize(text: str) -> str:
    return text.rstrip("\n\r \t")


def _render(prompts: dict[str, str]) -> str:
    lines = [
        "#include <string_view>",
        "",
        "namespace imza::prompts_detail {",
        "",
    ]
    for name in PROMPT_NAMES:
        lines.append(
            f"constexpr std::string_view {name.upper()} "
            f"= {_raw_cpp_string(_normalize(prompts[name]))};"
        )
    lines.extend(["", "} // namespace imza::prompts_detail", ""])
    return "\n".join(lines)


def _render_catalog(path: Path) -> str:
    value = _normalize(path.read_text(encoding="utf-8"))
    return "\n".join([
        "#include <string_view>",
        "",
        "namespace imza {",
        "",
        f"inline constexpr std::string_view EMBEDDED_MCP_CATALOG "
        f"= {_raw_cpp_string(value)};",
        "",
        "} // namespace imza",
        "",
    ])


def _render_banner(path: Path) -> str:
    banner = _normalize(path.read_text(encoding="utf-8"))
    return "\n".join([
        "#include <string_view>",
        "",
        "namespace imza::ui_detail {",
        "",
        f"constexpr std::string_view BANNER = {_raw_cpp_string(banner)};",
        "",
        "} // namespace imza::ui_detail",
        "",
    ])


def _write_if_changed(path: Path, content: str) -> None:
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--banner", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    prompts: dict[str, str] = {}
    for name in PROMPT_NAMES:
        path = args.source / f"{name}.md"
        if not path.is_file():
            parser.error(f"prompt source is missing: {path}")
        prompts[name] = path.read_text(encoding="utf-8")
    _write_if_changed(args.output / "prompt_defaults.inc", _render(prompts))

    _write_if_changed(args.output / "banner.inc", _render_banner(args.banner))
    _write_if_changed(
        args.output / "mcp_catalog.inc", _render_catalog(args.catalog))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
