#!/usr/bin/env python3
"""Session-store analyzer for Imza.

Computes the behavioral metrics behind docs/OBJECTVE_LUA.md against the
saved sessions directory, so the rollout measurement plan is one command
instead of a manual audit.

Usage:
    python3 scripts/session_stats.py [--dir ~/.local/share/imza/sessions]
                                     [--json] [--details]

Since phase 5 the model-facing roster is `lua`, `skill`, and `subagent`;
every other capability reaches the transcript as an entry in a lua call's
`dispatch_log`. Older sessions recorded those same actions as native tool
calls. This analyzer normalizes both into one op stream and reports each
metric as two cohorts, native and via-lua, so adoption reads as a column
instead of a diff between two separate runs.

Metrics the dispatch log cannot carry are omitted by design: it records
only binding name, canonical target, and success, so read line ranges,
shell exit codes, and pipeline structure are unrecoverable, and permission
denials never appear as entries at all.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import statistics
import sys

# Roster names and lua binding names folded onto one vocabulary.
NATIVE_OPS = {"find": "grep", "webfetch": "web_fetch",
              "websearch": "web_search"}
# Binding names that shipped before phase 5 and were later renamed; the
# dispatch log keeps whatever name the build in use recorded.
BINDING_ALIASES = {"sh": "shell", "webfetch": "web.fetch",
                   "websearch": "web.search"}
BINDING_OPS = {"todo.get": "todo", "todo.set": "todo",
               "web.fetch": "web_fetch", "web.search": "web_search",
               "file.insert": "insert", "file.edit": "edit",
               "file.write": "write"}
DIFF_ADD, DIFF_REMOVE = 2, 1
RESULT_KINDS = {0: "output", 1: "error", 2: "reject", 3: "cancel"}

# Mirrors shell_builtin_allowed() and shell_readonly_allowed() in
# src/tools/shell.cpp: builtins pass wholesale, catalog programs only with
# allowed flags. Keys are "program" or "program subcommand"; the bare
# program never matches a catalog entry that expects a subcommand.
# Approximate on purpose -- what matters here is whether a command has the
# shape of a read, not a security decision.
SHELL_BUILTINS = frozenset({
    "basename", "cat", "cd", "cmp", "cut", "dirname", "echo", "false",
    "file", "grep", "groups", "head", "id", "ls", "md5sum", "popd",
    "printenv", "printf", "pushd", "pwd", "readlink", "realpath",
    "sha256sum", "shasum", "stat", "strings", "tail", "tr", "true",
    "uname", "wc",
})
READONLY_CATALOG = {
    "top": "-b -n -p -u -d",
    "ps": "aux -ef -e -f -u -p --sort",
    "df": "-h -T -i -k -m --output",
    "du": "-s -h -k -m --max-depth",
    "free": "-h -m -g -b -t -w",
    "lsof": "-i -p -u -t -n -P",
    "date": "-u -R -I",
    "uptime": "-p -s",
    "make": "-n --dry-run",
    "which": "-a",
    "git status": "-s -b -v --short --branch --porcelain -u"
                  " --untracked-files",
    "git log": "-p -n --oneline --graph --all --stat --follow --decorate",
    "git diff": "--cached --stat --name-only --name-status --shortstat"
                " --summary -U --unified",
    "git show": "--stat --name-only --name-status --oneline",
    "git blame": "-L -w -M -C --line-porcelain",
    "git branch": "-a -r -v -vv --list --show-current --contains"
                  " --merged --no-merged",
    "git tag": "-l -n --list --contains --merged --no-merged --sort",
    "git remote": "-v --verbose --get-url",
    "git ls-files": "-s -o -m -c -d --others --modified --cached"
                    " --deleted --full-name",
    "git describe": "--tags --all --long --abbrev --contains",
    "git config": "--get --get-regexp --list --list-all",
    "systemctl status": "-l --no-pager",
    "docker ps": "-a -q -s --all --quiet --size",
    "docker images": "-a -q --all --quiet",
    "docker logs": "-f -t --follow --timestamps --tail",
    "docker inspect": "",
    "npm ls": "-g --global --depth",
    "npm outdated": "-g --global",
    "npm view": "",
    "pip list": "-o --outdated --format",
    "pip show": "-f --files",
}

def load_sessions(directory: pathlib.Path) -> list[dict]:
    sessions = []
    for path in sorted(directory.glob("*.json")):
        if path.name == ".index.json":
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


def call_args(item: dict) -> dict:
    try:
        value = json.loads(item.get("args") or "{}")
    except (json.JSONDecodeError, TypeError):
        return {}
    return value if isinstance(value, dict) else {}


def native_target(name: str, args: dict) -> str:
    if name == "shell":
        return str(args.get("command", ""))
    for key in ("path", "file_path", "pattern", "query", "url"):
        if key in args:
            return str(args[key])
    return ""


def diff_views(item: dict) -> list[dict]:
    """Every diff recorded on a call: the legacy single one and lua's list."""
    views = []
    single = item.get("diff")
    if isinstance(single, dict):
        views.append(single)
    plural = item.get("diffs")
    if isinstance(plural, list):
        views.extend(v for v in plural if isinstance(v, dict))
    return views


def session_ops(items: list[dict]) -> list[dict]:
    """One entry per performed action, tagged with the channel that ran it.

    A lua call expands into the binding calls it recorded; a native call is
    a single op. Position is the parent item's index in the session, so the
    two cohorts stay comparable in the drift table.
    """
    ops = []
    total = len(items)
    for index, item in enumerate(items):
        name = item.get("name", "?")
        position = index / total if total else 0.0
        if name == "lua":
            for entry in item.get("dispatch_log") or []:
                if not isinstance(entry, dict):
                    continue
                binding = BINDING_ALIASES.get(entry.get("binding", "?"),
                                              entry.get("binding", "?"))
                target = entry.get("target", "")
                ops.append({
                    "op": BINDING_OPS.get(binding, binding),
                    "binding": binding,
                    "source": "lua", "position": position, "target": target,
                    # The log carries no arguments, so a lua op is identified
                    # by binding plus canonical target.
                    "identity": f"{binding} {target}",
                    "ok": bool(entry.get("ok", True)),
                })
        else:
            ops.append({
                "op": NATIVE_OPS.get(name, name), "binding": name,
                "source": "native", "position": position,
                "target": native_target(name, call_args(item)),
                "identity": json.dumps(call_args(item), sort_keys=True),
                "ok": item.get("result_kind", 0) == 0,
            })
    return ops


def command_segments(command: str) -> list[list[str]]:
    """Word lists for every segment of a command string."""
    for separator in ("&&", "||", ";", "|", "\n"):
        command = command.replace(separator, "\0")
    return [words for words in (part.split() for part in command.split("\0"))
            if words]


def flag_allowed(flags: str, argument: str) -> bool:
    allowed = set(flags.split())
    if argument.startswith("--"):
        return argument in allowed
    if argument.startswith("-") and len(argument) > 2:
        return all(f"-{char}" in allowed for char in argument[1:])
    return argument in allowed


def is_readonly_shell(command: str) -> bool:
    segments = command_segments(command)
    if not segments:
        return False
    for words in segments:
        program = words[0]
        if program in SHELL_BUILTINS:
            continue
        subcommand = (words[1] if len(words) > 1
                      and not words[1].startswith("-") else None)
        flags = READONLY_CATALOG.get(
            f"{program} {subcommand}" if subcommand else program)
        if flags is None:
            return False
        for argument in words[1 + (1 if subcommand else 0):]:
            if not flag_allowed(flags, argument):
                return False
    return True


def analyze(sessions: list[dict]) -> dict:
    roster_mix: collections.Counter = collections.Counter()
    op_mix: dict[str, collections.Counter] = collections.defaultdict(
        collections.Counter)
    positions: dict[tuple[str, str], list[float]] = \
        collections.defaultdict(list)
    outcomes: dict[str, collections.Counter] = collections.defaultdict(
        collections.Counter)
    shell: collections.Counter = collections.Counter()
    repeated: collections.Counter = collections.Counter()
    binding_calls: collections.Counter = collections.Counter()
    binding_failures: collections.Counter = collections.Counter()
    binding_targets: dict[str, set] = collections.defaultdict(set)
    ops_per_lua_call: list[int] = []
    lua_diff_calls = lua_calls_with_log = truncated_lua = 0
    diff_rows = {"added": 0, "removed": 0}
    files_touched: collections.Counter = collections.Counter()
    late_shell_sessions = measured_sessions = 0
    per_session = []

    for session in sessions:
        items = tool_items(session)
        names = [m.get("name", "?") for m in items]
        roster_mix.update(names)
        ops = session_ops(items)

        for item in items:
            name = item.get("name", "?")
            outcomes[name][RESULT_KINDS.get(item.get("result_kind"),
                                            "no result")] += 1
            if name != "lua":
                continue
            log_len = sum(1 for e in (item.get("dispatch_log") or [])
                          if isinstance(e, dict))
            ops_per_lua_call.append(log_len)
            if log_len:
                lua_calls_with_log += 1
            if diff_views(item):
                lua_diff_calls += 1
            if "[truncated]" in (item.get("result") or ""):
                truncated_lua += 1

        for op in ops:
            op_mix[op["op"]][op["source"]] += 1
            positions[(op["op"], op["source"])].append(op["position"])
            if op["source"] == "lua":
                binding_calls[op["binding"]] += 1
                if not op["ok"]:
                    binding_failures[op["binding"]] += 1
                if op["target"]:
                    binding_targets[op["binding"]].add(op["target"])
            if op["op"] == "shell":
                shell["total"] += 1
                shell[op["source"]] += 1
                shell["readonly" if is_readonly_shell(op["target"])
                      else "mutating"] += 1

        for item in items:
            for view in diff_views(item):
                files_touched[view.get("file", "?")] += 1
                for row in view.get("rows") or []:
                    if not isinstance(row, dict):
                        continue
                    if row.get("kind") == DIFF_ADD:
                        diff_rows["added"] += 1
                    elif row.get("kind") == DIFF_REMOVE:
                        diff_rows["removed"] += 1

        # Consecutive identical actions, comparable only within a cohort:
        # a native op is identified by its exact argument string, a lua op by
        # binding plus canonical target, since the log records no line ranges
        # and no script text. Target-less ops (todo, ask) carry nothing worth
        # comparing and break the run.
        prev = None
        for op in ops:
            key = ((op["source"], op["op"], op["identity"])
                   if op["target"] else None)
            if key is not None and key == prev:
                repeated[op["source"]] += 1
            prev = key

        n = len(ops)
        if n >= 20:
            measured_sessions += 1
            tail = ops[2 * n // 3:]
            shells = sum(1 for o in tail if o["op"] == "shell")
            if shells > 0.5 * len(tail) and shells >= 5:
                late_shell_sessions += 1

        lua_ops = sum(1 for o in ops if o["source"] == "lua")
        per_session.append({
            "file": session["path"], "roster_calls": len(items), "ops": n,
            "lua_calls": names.count("lua"), "lua_ops": lua_ops,
            "lua_op_share": round(lua_ops / n, 2) if n else 0.0,
        })

    total_ops = sum(sum(counts.values()) for counts in op_mix.values())
    lua_total = sum(binding_calls.values())
    drift = {f"{op}|{source}": round(statistics.mean(pos), 2)
             for (op, source), pos in sorted(positions.items()) if pos}
    return {
        "sessions_analyzed": len(sessions),
        "roster_calls": sum(roster_mix.values()),
        "roster_mix": dict(roster_mix.most_common()),
        "ops": {
            "total": total_ops,
            "via_lua": lua_total,
            "mix": {op: dict(sources) for op, sources in sorted(
                op_mix.items(), key=lambda kv: -sum(kv[1].values()))},
        },
        "outcomes": {name: dict(counts)
                     for name, counts in sorted(outcomes.items())},
        "shell": {
            "total": shell["total"],
            "native": shell["native"], "via_lua": shell["lua"],
            "readonly_catalog_shaped": shell["readonly"],
            "mutating_or_unknown": shell["mutating"],
            "readonly_share": round(shell["readonly"] / shell["total"], 2)
            if shell["total"] else 0.0,
        },
        "drift_mean_position": drift,
        "late_session_shell_takeover": late_shell_sessions,
        "sessions_over_20_ops": measured_sessions,
        "immediately_repeated_ops": dict(repeated),
        "lua": {
            "calls": roster_mix.get("lua", 0),
            "calls_with_binding_calls": lua_calls_with_log,
            "calls_with_diffs": lua_diff_calls,
            "truncated_results": truncated_lua,
            "binding_calls": dict(binding_calls.most_common()),
            "binding_failures": dict(binding_failures.most_common()),
            "distinct_targets": {b: len(t)
                                 for b, t in sorted(binding_targets.items())},
            "ops_per_call": {
                "mean": round(statistics.mean(ops_per_lua_call), 1)
                if ops_per_lua_call else 0.0,
                "max": max(ops_per_lua_call) if ops_per_lua_call else 0,
            },
        },
        "diff": {
            "files_touched": len(files_touched),
            "added_rows": diff_rows["added"],
            "removed_rows": diff_rows["removed"],
            "most_rewritten_files": dict(files_touched.most_common(10)),
        },
        "adoption": {
            "lua_op_share": round(lua_total / total_ops, 2)
            if total_ops else 0.0,
            "sessions_with_lua_ops": sum(1 for s in per_session
                                               if s["lua_ops"]),
            "native_only_sessions": sum(1 for s in per_session
                                        if not s["lua_ops"]),
        },
        "per_session_sizes": per_session,
    }


def print_report(stats: dict, details: bool) -> None:
    print(f"sessions analyzed:  {stats['sessions_analyzed']}")
    print(f"roster tool calls:  {stats['roster_calls']}")

    print("\n== roster mix (what the model called) ==")
    for name, count in stats["roster_mix"].items():
        print(f"  {name:10} {count}")

    ops = stats["ops"]
    print(f"\n== normalized ops ({ops['total']} performed actions,"
          f" {ops['via_lua']} of them via lua) ==")
    print(f"  {'op':<12}{'native':>8}{'via-lua':>9}{'total':>8}")
    for op, sources in ops["mix"].items():
        native = sources.get("native", 0)
        lua = sources.get("lua", 0)
        print(f"  {op:<12}{native:>8}{lua:>9}{native + lua:>8}")

    shell = stats["shell"]
    print(f"\n== shell ({shell['total']} calls: {shell['native']} native,"
          f" {shell['via_lua']} via binding) ==")
    print(f"  readonly-catalog-shaped: {shell['readonly_catalog_shaped']}"
          f" ({shell['readonly_share']:.0%})")
    print(f"  mutating/unknown:        {shell['mutating_or_unknown']}")

    print("\n== positional drift (0=start 1=end) ==")
    print(f"  {'op':<12}{'native':>8}{'via-lua':>9}")
    drift = stats["drift_mean_position"]
    for op in ops["mix"]:
        native, lua = drift.get(f"{op}|native"), drift.get(f"{op}|lua")
        if native is None and lua is None:
            continue
        print(f"  {op:<12}"
              f"{f'{native:.2f}' if native is not None else '-':>8}"
              f"{f'{lua:.2f}' if lua is not None else '-':>9}")
    print(f"  sessions with late shell takeover: "
          f"{stats['late_session_shell_takeover']}"
          f" of {stats['sessions_over_20_ops']} sessions over 20 ops")

    lua = stats["lua"]
    print(f"\n== lua ({lua['calls']} calls,"
          f" {lua['calls_with_binding_calls']} with recorded binding"
          " calls) ==")
    print(f"  binding calls per lua call: mean {lua['ops_per_call']['mean']}"
          f", max {lua['ops_per_call']['max']}")
    print(f"  {'binding':<12}{'calls':>7}{'failed':>8}{'targets':>9}")
    for binding, count in lua["binding_calls"].items():
        print(f"  {binding:<12}{count:>7}"
              f"{lua['binding_failures'].get(binding, 0):>8}"
              f"{lua['distinct_targets'].get(binding, 0):>9}")
    print(f"  calls with recorded diffs: {lua['calls_with_diffs']}")
    print(f"  truncated results:         {lua['truncated_results']}")

    print("\n== call outcomes ==")
    print(f"  {'tool':<12}{'output':>8}{'error':>7}{'reject':>8}"
          f"{'cancel':>8}{'open':>6}")
    for name, counts in stats["outcomes"].items():
        print(f"  {name:<12}{counts.get('output', 0):>8}"
              f"{counts.get('error', 0):>7}{counts.get('reject', 0):>8}"
              f"{counts.get('cancel', 0):>8}"
              f"{counts.get('no result', 0):>6}")

    print("\n== mutation discipline ==")
    repeated = stats["immediately_repeated_ops"]
    print(f"  immediately repeated identical ops: native "
          f"{repeated.get('native', 0)}, lua {repeated.get('lua', 0)}")
    diff = stats["diff"]
    print(f"  distinct files with diffs:          {diff['files_touched']}")
    print(f"  diff rows added/removed:            "
          f"{diff['added_rows']}/{diff['removed_rows']}")
    if details:
        print("  most rewritten files:")
        for path, count in diff["most_rewritten_files"].items():
            print(f"    {count:4}  {path}")

    adoption = stats["adoption"]
    print("\n== adoption ==")
    print(f"  lua share of all ops:   {adoption['lua_op_share']:.0%}")
    print(f"  sessions with lua ops:  {adoption['sessions_with_lua_ops']}")
    print(f"  native-only sessions:   {adoption['native_only_sessions']}")
    if details:
        print("  per session:")
        print(f"    {'file':<38}{'roster':>7}{'ops':>6}{'lua':>5}{'share':>7}")
        for row in stats["per_session_sizes"]:
            print(f"    {row['file']:<38}{row['roster_calls']:>7}"
                  f"{row['ops']:>6}{row['lua_calls']:>5}"
                  f"{row['lua_op_share']:>7.0%}")


def main() -> int:
    default_dir = pathlib.Path.home() / ".local/share/imza/sessions"
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
