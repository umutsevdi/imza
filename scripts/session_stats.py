#!/usr/bin/env python3
"""Session-store analyzer for Imza.

Computes the behavioral metrics referenced by docs/OBJECTVE_LUA.md
against the saved sessions directory, so the rollout measurement plan
(phase 2 / phase 5) is one command instead of a manual audit.

Usage:
    python3 scripts/session_stats.py [--dir ~/.local/share/imza/sessions]
                                     [--json] [--details]

The tool mix, piped-shell classification, positional drift, and retry
figures printed here mirror the tables in the objective document; run
it before and after dogfooding `run_lua` and compare.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import statistics
import sys

READONLY_SHELL_MARKERS = ("grep", "rg ", "head", "tail", "cat", "ls",
                          "git status", "git diff", "git log", "sed -n",
                          "find ", "wc ", "true")


def load_sessions(directory: pathlib.Path) -> list[dict]:
    sessions = []
    for path in sorted(directory.glob("*.json")):
        if path.name.endswith(".lock"):
            continue
        try:
            data = json.loads(path.read_text())
        except (json.JSONDecodeError, OSError):
            continue
        items = data.get("items")
        if not isinstance(items, list):
            continue
        sessions.append({"path": path.name, "items": items,
                         "mode": data.get("mode", "?")})
    return sessions


def tool_items(session: dict) -> list[dict]:
    return [m for m in session["items"]
            if isinstance(m, dict) and m.get("type") == "tool"]


def tool_command(item: dict) -> str:
    try:
        return json.loads(item.get("args", "{}")).get("command", "")
    except (json.JSONDecodeError, TypeError):
        return ""


def classify_pipe(command: str) -> str:
    lowered = f" {command} "
    if "| grep" in command or "| rg " in command:
        if "error" in lowered or "warn" in lowered or "FAILED" in lowered:
            return "build/test output filtered"
        return "grep filter"
    if "| head" in command or command.startswith("head "):
        return "head truncation"
    if "| tail" in command or command.startswith("tail "):
        return "tail truncation"
    return "other"


def is_readonly_shell(command: str) -> bool:
    first = command.strip().split("&&")[0].split(";")[0].split("|")[0]
    return any(marker in first for marker in READONLY_SHELL_MARKERS)


def analyze(sessions: list[dict]) -> dict:
    mix: collections.Counter = collections.Counter()
    pipe_cats: collections.Counter = collections.Counter()
    positions: dict[str, list[float]] = collections.defaultdict(list)
    late_shell_sessions = 0
    ranged_reads = whole_reads = 0
    shell_total = piped = readonly_shell = mutating_shell = 0
    exit_codes: collections.Counter = collections.Counter()
    timeouts = 0
    repeated_calls = 0
    tool_calls_total = 0
    diff_lines = {"added": 0, "removed": 0}
    files_touched: collections.Counter = collections.Counter()
    per_session = []

    for session in sessions:
        items = tool_items(session)
        names = [m.get("name", "?") for m in items]
        n = len(names)
        tool_calls_total += n
        mix.update(names)
        per_session.append({"file": session["path"], "tools": n})

        if n >= 20:
            last_third = names[2 * n // 3:]
            last_counts = collections.Counter(last_third)
            if (last_counts.get("shell", 0)
                    > 0.5 * len(last_third) and last_counts["shell"] >= 5):
                late_shell_sessions += 1

        prev = None
        for i, item in enumerate(items):
            name = item.get("name", "?")
            rel = i / n if n else 0.0
            if name in ("shell", "read", "edit", "find", "write", "list",
                        "run_lua"):
                positions[name].append(rel)

            if name == "shell":
                shell_total += 1
                command = tool_command(item)
                if "|" in command:
                    piped += 1
                    pipe_cats[classify_pipe(command)] += 1
                if is_readonly_shell(command):
                    readonly_shell += 1
                else:
                    mutating_shell += 1
                if item.get("shell_exit") is not None:
                    exit_codes[item["shell_exit"]] += 1
                if item.get("shell_timeout"):
                    timeouts += 1

            if name == "read":
                try:
                    args = json.loads(item.get("args", "{}"))
                except (json.JSONDecodeError, TypeError):
                    args = {}
                if "line_begin" in args:
                    ranged_reads += 1
                else:
                    whole_reads += 1

            diff = item.get("diff")
            if isinstance(diff, dict):
                files_touched[diff.get("file", "?")] += 1
                for row in diff.get("rows", []):
                    kind = row.get("kind")
                    if kind == 1:
                        diff_lines["added"] += 1
                    elif kind == 2:
                        diff_lines["removed"] += 1

            key = (name, item.get("args"))
            if key == prev:
                repeated_calls += 1
            prev = key

    drift = {name: round(statistics.mean(pos), 2)
             for name, pos in sorted(positions.items()) if pos}
    return {
        "sessions_analyzed": len(sessions),
        "tool_calls": tool_calls_total,
        "tool_mix": dict(mix.most_common()),
        "shell": {
            "total": shell_total,
            "piped": piped,
            "piped_share": round(piped / shell_total, 2) if shell_total else 0,
            "pipe_categories": dict(pipe_cats.most_common()),
            "readonly_catalog_shaped": readonly_shell,
            "mutating_or_unknown": mutating_shell,
            "exit_codes": dict(exit_codes.most_common()),
            "timeouts": timeouts,
        },
        "reads": {"ranged": ranged_reads, "whole_file": whole_reads},
        "drift_mean_position": drift,
        "late_session_shell_takeover": late_shell_sessions,
        "immediately_repeated_calls": repeated_calls,
        "diff": {
            "edit_calls_with_diff_rows": files_touched.total(),
            "added_rows": diff_lines["added"],
            "removed_rows": diff_lines["removed"],
            "most_rewritten_files": dict(
                files_touched.most_common(10)),
        },
        "per_session_sizes": per_session,
    }


def print_report(stats: dict, details: bool) -> None:
    print(f"sessions analyzed:        {stats['sessions_analyzed']}")
    print(f"tool calls:               {stats['tool_calls']}")
    print("\n== tool mix ==")
    for name, count in stats["tool_mix"].items():
        print(f"  {name:10} {count}")

    shell = stats["shell"]
    print(f"\n== shell ({shell['total']} calls) ==")
    print(f"  piped: {shell['piped']} ({shell['piped_share']:.0%})")
    for category, count in shell["pipe_categories"].items():
        print(f"    {category:28} {count}")
    print(f"  readonly-catalog-shaped: {shell['readonly_catalog_shaped']}")
    print(f"  mutating/unknown:        {shell['mutating_or_unknown']}")
    print(f"  timeouts:                {shell['timeouts']}")
    print("  exit codes:              "
          + ", ".join(f"{code}x{count}"
                      for code, count in shell["exit_codes"].items()))

    print(f"\n== reads ==")
    print(f"  ranged:     {stats['reads']['ranged']}")
    print(f"  whole-file: {stats['reads']['whole_file']}")

    print("\n== positional drift (0=start 1=end) ==")
    for name, mean in stats["drift_mean_position"].items():
        print(f"  {name:8} {mean:.2f}")
    print(f"  sessions with late shell takeover: "
          f"{stats['late_session_shell_takeover']}")

    print("\n== mutation discipline ==")
    print(f"  immediately repeated identical calls: "
          f"{stats['immediately_repeated_calls']}")
    print(f"  edit calls with recorded diffs:       "
          f"{stats['diff']['edit_calls_with_diff_rows']}")
    print(f"  diff rows added/removed:              "
          f"{stats['diff']['added_rows']}/"
          f"{stats['diff']['removed_rows']}")
    if details:
        print("  most rewritten files:")
        for path, count in stats["diff"]["most_rewritten_files"].items():
            print(f"    {count:4}  {path}")


def main() -> int:
    default_dir = (pathlib.Path.home() / ".local/share/imza/sessions")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=pathlib.Path, default=default_dir,
                        help="sessions directory")
    parser.add_argument("--json", action="store_true",
                        help="emit machine-readable JSON")
    parser.add_argument("--details", action="store_true",
                        help="include per-file breakdowns")
    args = parser.parse_args()

    if not args.dir.is_dir():
        print(f"error: {args.dir} is not a directory", file=sys.stderr)
        return 2

    stats = analyze(load_sessions(args.dir))
    if args.json:
        print(json.dumps(stats, indent=2))
    else:
        print_report(stats, args.details)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
