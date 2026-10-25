#include "tools/bindings.h"

#include <chrono>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    // Runs a command detached, in its own process group, so it outlives the
    // tool call and a later script can interact with it. Output is not
    // captured: the command redirects it itself.
    int binding_bg_shell(lua_State* L)
    {
        ShellRequestArgs args;
        if (const int failed = resolve_shell_request(L, "bg.shell",
                "compose results in Lua instead of chaining with && || ; |",
                DEFAULT_BG_SECONDS, MAX_TASK_SECONDS, args);
            failed != 0) {
            return failed;
        }

        LuaRunContext* run = run_of(L);
        if (!run->host->spawn_bg) {
            return binding_error(L, "bg.shell: unavailable in this context");
        }
        if (!run->host->spawn_bg(args.command,
                std::chrono::seconds(args.timeout), args.run_dir,
                args.run_env)) {
            record_call(L, "bg.shell", args.command, false);
            return binding_error(L, "bg.shell: failed to start command");
        }
        record_call(L, "bg.shell", args.command, true);
        return 0;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "shell",
            binding_bg_shell,
            R"desc((command: string, timeout?: integer=120, workspace?: string,
env?: table<string, string>) throws
Starts a command in the background and returns as soon as it is running, so
it outlives this script and a later script can interact with it (e.g. start
a server, then curl it). Output is not captured: redirect it in the command
(`cmd > log 2>&1`) and read the file with imza.fs.read.
The process ends on its own exit, after `timeout` seconds (1..600), or when
the application exits. Throws when the command could not start or was denied.
Chains and pipelines are rejected, like imza.shell. `env` maps variable
names to values, merged over the inherited environment.)desc",
            LuaCapability::SHELL,
            "bg.shell: shell access is disabled for this run",
        },
    };

} // namespace

const LuaModule& bg_module()
{
    static constexpr LuaModule MODULE { true, "bg",
        "Execute tasks in the background", { }, BINDINGS };
    return MODULE;
}

} // namespace imza
