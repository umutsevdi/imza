#!/usr/bin/env python3
"""Compare terminal coding agents on startup cost.

Every agent runs in the same empty directory, so none pays for scanning a
workspace the others cannot see. Nothing here measures model latency; that
belongs to the provider, not the tool.

Metrics, per agent, as the median of N passes:

  launch        exec -> process alive. The cost of bringing the runtime up.
  first frame   exec -> first byte drawn. What the user sees first. For some
                tools this is a banner or terminal setup rather than a usable
                prompt, so read it together with `idle`.
  pss first     proportional set size at the first frame.
  rss first     resident set size at the first frame (what top shows).
  pss idle      proportional set size once memory has stopped growing.
                Sampled until it plateaus, so a tool that lazy-loads after
                drawing is charged for what it eventually uses instead of
                what it used at the moment it looked ready.
  disk          installed footprint: the executable, or for a script agent
                the package directory it cannot run without.

PSS is reported beside RSS because it divides shared pages among the
processes that map them; RSS charges each process for the whole page even
when the system already had it loaded.

Each agent runs inside a private PID namespace (bwrap), and memory is summed
over its whole process tree. Both matter: several agents hand work to a
background daemon that re-parents to init, so without the namespace the
daemon outlives the run, the next run attaches to it warm, and the daemon's
memory is attributed to nothing. Measured that way codex looked like 51 MB
and cline like 21 MB; counted properly they are 76 MB and 419 MB. Without
bwrap installed the script still runs, but daemon-spawning agents are
undercounted.

Agents launch with the flags in AGENT_ARGS so none is measured parked on a
first-run gate (folder-trust prompts, onboarding): the bench never sends
keystrokes, so a tool waiting on one would have its post-gate loading go
uncounted.

Usage:
    scripts/agent_bench.py                        # imza, if built
    scripts/agent_bench.py --all                  # every known agent on PATH
    scripts/agent_bench.py imza crush codex       # named agents from PATH
    scripts/agent_bench.py --runs 5 --json
    scripts/agent_bench.py build-release/release/imza

Run against a Release build; a Debug build is not representative.
"""

from __future__ import annotations

import argparse
import json
import os
import pty
import select
import shutil
import subprocess
import sys
import time

# Names discovered by --all, in the order they are reported.
KNOWN_AGENTS = ("imza", "claude", "codex", "crush", "opencode",
                "gemini", "cline", "pi")

DEFAULT_BINARY = "build-release/release/imza"
DEFAULT_WORKDIR = "/tmp/imza-bench"
PAGE_KB = 1024.0
GROWTH_KB = 256.0


def read_status(pid: int, key: str) -> int:
    """One field from /proc/<pid>/status in kB, or 0 if unavailable."""
    try:
        with open(f"/proc/{pid}/status") as fh:
            for line in fh:
                if line.startswith(key):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def read_pss(pid: int) -> int:
    """Proportional set size in kB, or 0 if unavailable."""
    try:
        with open(f"/proc/{pid}/smaps_rollup") as fh:
            for line in fh:
                if line.startswith("Pss:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0

def process_table() -> dict:
    """pid -> (ppid, comm) for every process on the system."""
    procs = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        try:
            ppid = 0
            with open(f"/proc/{pid}/status") as fh:
                for line in fh:
                    if line.startswith("PPid:"):
                        ppid = int(line.split()[1])
                        break
            with open(f"/proc/{pid}/comm") as fh:
                comm = fh.read().strip()
            procs[pid] = (ppid, comm)
        except OSError:
            continue
    return procs


def tree_pss(root: int) -> int:
    """PSS summed over root and every descendant, in kB.

    An agent that spawns a daemon keeps it as a descendant (bwrap's namespace
    init is the re-parent target), so a tree walk charges the tool for the
    work it delegated instead of only the process we launched.
    """
    procs = process_table()
    members = {root}
    changed = True
    while changed:
        changed = False
        for pid, (ppid, _) in procs.items():
            if ppid in members and pid not in members:
                members.add(pid)
                changed = True
    return sum(read_pss(pid) for pid in members)

def tree_rss(root: int) -> int:
    """RSS summed over root and every descendant, in kB."""
    procs = process_table()
    members = {root}
    changed = True
    while changed:
        changed = False
        for pid, (ppid, _) in procs.items():
            if ppid in members and pid not in members:
                members.add(pid)
                changed = True
    return sum(read_status(pid, "VmRSS:") for pid in members)


def disk_footprint(command: str) -> int:
    """Bytes the agent occupies on disk.

    A compiled agent is one file, so its size is the answer. A script agent
    is a few kilobytes of JavaScript that cannot run without the package
    shipped beside it, so report that package directory; charging it only
    for the script would flatter it by two orders of magnitude.
    """
    real = os.path.realpath(shutil.which(command) or command)
    if not os.path.exists(real):
        return 0

    with open(real, "rb") as fh:
        if fh.read(2) != b"#!":
            return os.path.getsize(real)

    directory = os.path.dirname(real)
    while directory not in ("/", ""):
        if os.path.exists(os.path.join(directory, "package.json")):
            total = 0
            for root, _, files in os.walk(directory):
                for name in files:
                    try:
                        total += os.path.getsize(os.path.join(root, name))
                    except OSError:
                        pass
            return total
        directory = os.path.dirname(directory)
    return os.path.getsize(real)


def resolve(command: str) -> str:
    """Absolute path to the agent.

    The child runs with cwd set to the shared workspace, so a relative path
    would be looked up there and fail. Resolve against the caller's cwd.
    """
    if os.path.sep in command:
        return os.path.abspath(command)
    return shutil.which(command) or command


# Startup flags that put an agent in its normal working state. Without
# --skip-trust gemini parks on its folder-trust gate in the fresh bench
# workspace (nothing answers the prompt, so trust is never recorded) and
# never loads the rest of itself, undercounting idle memory. Keyed by the
# resolved executable name.
AGENT_ARGS = {
    "gemini": ["--skip-trust"],
}


def agent_command(command: str) -> list[str]:
    """The agent invocation with benchmark-state flags folded in."""
    return [command, *AGENT_ARGS.get(os.path.basename(command), [])]


def sandboxed(command: str, workdir: str) -> list[str]:
    """Wrap the agent so nothing it spawns outlives the measurement.

    Some agents hand work to a background daemon that re-parents to init and
    survives the launcher. That breaks the benchmark twice: the daemon's
    memory is invisible to a tree walk, and the next run attaches to the warm
    daemon instead of starting cold, so repeated runs look faster and lighter
    than a first run. A private PID namespace reaps everything inside when
    the sandbox exits, so every run starts from the same state and is charged
    for everything it uses.

    PATH and HOME are passed through explicitly: without them bwrap leaves
    the agent with no PATH and it exits immediately. HOME is bound read-write
    because agents write config on first run. /tmp is a fresh tmpfs because
    some agents extract a native library there at startup and fail if it is
    read-only; the shared workspace is bound back in afterwards.

    Falls back to running the agent directly if bwrap is unavailable, with
    the caveat that daemon-spawning agents will then be undercounted.
    """
    if not shutil.which("bwrap"):
        return agent_command(command)
    home = os.path.expanduser("~")
    return [
        "bwrap",
        "--unshare-pid", "--unshare-ipc", "--unshare-uts",
        "--die-with-parent",
        "--ro-bind", "/", "/",
        "--dev", "/dev",
        "--proc", "/proc",
        # A writable /tmp: some agents extract a native library there at
        # startup and fail outright if it is read-only. The shared workspace
        # is bound back in afterwards, since it usually lives under /tmp.
        "--tmpfs", "/tmp",
        "--bind", workdir, workdir,
        "--bind", home, home,
        "--setenv", "PATH", os.environ.get("PATH", ""),
        "--setenv", "HOME", home,
        "--setenv", "TERM", "xterm-256color",
        "--chdir", workdir,
        *agent_command(command),
    ]


def run_once(command: str, workdir: str, settle: float, quiet: float,
             spawn_timeout: float):
    """One pass. Returns a sample dict, or None if no frame was drawn."""
    master, slave = pty.openpty()
    start = time.perf_counter()
    try:
        proc = subprocess.Popen(
            sandboxed(command, workdir), cwd=workdir,
            stdin=slave, stdout=slave, stderr=slave,
            close_fds=True, env={**os.environ, "TERM": "xterm-256color"})
    except OSError:
        os.close(master)
        os.close(slave)
        return None
    os.close(slave)

    try:
        launch_ms = (time.perf_counter() - start) * 1000.0

        first = None
        pss_first = rss_first = 0
        deadline = start + spawn_timeout
        while time.perf_counter() < deadline:
            ready, _, _ = select.select([master], [], [], 0.05)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    break
                if data and first is None:
                    first = time.perf_counter()
                    pss_first = tree_pss(proc.pid)
                    rss_first = tree_rss(proc.pid)
            if proc.poll() is not None:
                break

        if first is None:
            return None

        # Watch memory until it stops growing, so lazy loading is counted.
        # Keep draining the terminal throughout: a tool that blocks on a full
        # output buffer never reaches the point where it loads the rest of
        # itself, and would be measured at its first-frame footprint instead.
        pss_idle = pss_first
        last_growth = time.perf_counter()
        while time.perf_counter() - start < settle:
            ready, _, _ = select.select([master], [], [], 0.25)
            if ready:
                try:
                    os.read(master, 65536)
                except OSError:
                    break
            if proc.poll() is not None:
                break
            current = tree_pss(proc.pid)
            if current > pss_idle + GROWTH_KB:
                pss_idle = current
                last_growth = time.perf_counter()
            elif time.perf_counter() - last_growth >= quiet:
                break
            pss_idle = max(pss_idle, current)

        # Both readings at the settled moment, so the pair describes the same
        # state rather than two different points in the startup.
        rss_idle = tree_rss(proc.pid) if proc.poll() is None else rss_first

        return {
            "launch_ms": launch_ms,
            "first_frame_ms": (first - start) * 1000.0,
            "pss_first_kb": pss_first,
            "rss_first_kb": rss_first,
            "pss_idle_kb": pss_idle,
            "rss_idle_kb": rss_idle,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        os.close(master)


def median(values):
    values = sorted(values)
    return values[len(values) // 2]


def measure(command: str, args) -> tuple[dict | None, str | None]:
    """Median of `runs` passes, or an error string."""
    resolved = resolve(command)
    samples = []
    for _ in range(args.runs):
        sample = run_once(resolved, args.workdir, args.settle, args.quiet,
                          args.spawn_timeout)
        if sample:
            samples.append(sample)
    if not samples:
        return None, "never drew a frame"

    return {
        "agent": os.path.basename(command),
        "launch_ms": median([s["launch_ms"] for s in samples]),
        "first_frame_ms": median([s["first_frame_ms"] for s in samples]),
        "pss_first_mb": median([s["pss_first_kb"] / PAGE_KB for s in samples]),
        "rss_first_mb": median([s["rss_first_kb"] / PAGE_KB for s in samples]),
        "pss_idle_mb": median([s["pss_idle_kb"] / PAGE_KB for s in samples]),
        "rss_idle_mb": median([s["rss_idle_kb"] / PAGE_KB for s in samples]),
        "disk_mb": disk_footprint(command) / (1024.0 * 1024.0),
        "runs": len(samples),
    }, None


def print_table(rows: list[dict]) -> None:
    header = (f"{'agent':<10} {'launch':>9} {'first frame':>12} "
              f"{'pss first':>10} {'rss first':>10} "
              f"{'pss idle':>9} {'rss idle':>9} {'disk':>9}")
    print(header)
    print("-" * len(header))
    for r in rows:
        print(f"{r['agent']:<10} {r['launch_ms']:>6.0f} ms "
              f"{r['first_frame_ms']:>9.0f} ms "
              f"{r['pss_first_mb']:>7.1f} MB {r['rss_first_mb']:>7.1f} MB "
              f"{r['pss_idle_mb']:>6.1f} MB {r['rss_idle_mb']:>6.1f} MB "
              f"{r['disk_mb']:>6.0f} MB")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare startup cost across terminal coding agents.")
    parser.add_argument("agents", nargs="*",
                        help="agent names or paths; default: built imza")
    parser.add_argument("--all", action="store_true",
                        help="benchmark every known agent found on PATH")
    parser.add_argument("--runs", type=int, default=3,
                        help="passes per agent (default 3)")
    parser.add_argument("--settle", type=float, default=15.0,
                        help="seconds to watch memory settle (default 15)")
    parser.add_argument("--quiet", type=float, default=2.0,
                        help="seconds without growth to call it settled")
    parser.add_argument("--spawn-timeout", type=float, default=8.0,
                        help="seconds to wait for the first frame")
    parser.add_argument("--workdir", default=DEFAULT_WORKDIR,
                        help=f"empty workspace for every agent "
                             f"(default {DEFAULT_WORKDIR})")
    parser.add_argument("--json", action="store_true",
                        help="emit JSON instead of a table")
    args = parser.parse_args()

    if args.all:
        agents = [n for n in KNOWN_AGENTS if shutil.which(n)]
    elif args.agents:
        agents = args.agents
    elif os.path.exists(DEFAULT_BINARY):
        agents = [DEFAULT_BINARY]
    else:
        print(f"no agents given and {DEFAULT_BINARY} is not built; build it "
              f"with:\n  cmake -B build-release -DCMAKE_BUILD_TYPE=Release\n"
              f"  cmake --build build-release -j --target imza",
              file=sys.stderr)
        return 1

    if not agents:
        print("no agents found; name one explicitly", file=sys.stderr)
        return 1

    os.makedirs(args.workdir, exist_ok=True)

    rows, failures = [], []
    for agent in agents:
        result, error = measure(agent, args)
        if result:
            rows.append(result)
        else:
            failures.append((os.path.basename(agent), error))

    if args.json:
        print(json.dumps({
            "results": rows,
            "failures": [{"agent": a, "error": e} for a, e in failures],
        }, indent=2))
        return 0

    print(f"workspace: {args.workdir} (empty)   "
          f"runs: {args.runs} (medians)\n")
    print_table(rows)
    for name, error in failures:
        print(f"{name:<10} {error}")
    print("\npss divides shared pages among the processes that map them; "
          "rss charges each process whole.")
    print("disk counts a script agent's package directory, not just its "
          "entry script.")
    if not shutil.which("bwrap"):
        print("warning: bwrap not found; agents that spawn a background "
              "daemon will be undercounted.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
