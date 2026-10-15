#include "tools/bindings.h"

#include "common/util.h"
#include "permissions/shell.h"
#include "permissions/shell_analysis.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    namespace fs = std::filesystem;

    // Runs a command detached, in its own process group, so it outlives the
    // tool call and a later script can interact with it. Output is not
    // captured: the command redirects it itself.
    int binding_bg_shell(lua_State* L)
    {
        const std::string command = luaL_checkstring(L, 1);
        const long timeout
            = std::clamp<long>(opt_integer(L, 2).value_or(DEFAULT_BG_SECONDS),
                1, MAX_TASK_SECONDS);

        std::string workspace;
        if (const auto given = opt_string(L, 3)) {
            if (const int failed = resolve_workspace(L, given, workspace);
                failed != 0) {
                return failed;
            }
        }

        const ShellAnalysis analysis = analyze_shell(command);
        if (analysis.invocations.size() > 1) {
            return binding_error(L,
                "bg.shell: one command per call; compose results in Lua "
                "instead of chaining with && || ; |");
        }
        if (analysis.invocations.empty()) {
            return binding_error(L, "bg.shell: empty command");
        }

        const GateOutcome gate = authorize_shell(L,
            ShellRequest {
                command, std::chrono::seconds(timeout), fs::path(workspace) });
        if (!gate) {
            return binding_error(L, gate.denial);
        }
        const std::string run_dir
            = gate.shell ? gate.shell->workspace.string() : workspace;

        LuaRunContext* run = run_of(L);
        if (!run->host->spawn_bg) {
            return binding_error(L, "bg.shell: unavailable in this context");
        }
        if (!run->host->spawn_bg(
                command, std::chrono::seconds(timeout), run_dir)) {
            record_call(L, "bg.shell", command, false);
            return binding_error(L, "bg.shell: failed to start command");
        }
        record_call(L, "bg.shell", command, true);
        return 0;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "shell",
            binding_bg_shell,
            R"desc((command: string, timeout?: integer=120, workspace?: string) throws
Starts a command in the background and returns as soon as it is running, so
it outlives this script and a later script can interact with it (e.g. start
a server, then curl it). Output is not captured: redirect it in the command
(`cmd > log 2>&1`) and read the file with imza.fs.read.
The process ends on its own exit, after `timeout` seconds (1..600), or when
the application exits. Throws when the command could not start or was denied.
Chains and pipelines are rejected, like imza.shell.)desc",
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
