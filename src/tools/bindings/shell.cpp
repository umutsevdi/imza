#include "tools/bindings.h"

#include "permissions/shell.h"
#include "permissions/shell_analysis.h"
#include "platform/command_runner.h"
#include "workspace/environment.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    namespace fs = std::filesystem;

    int binding_shell(lua_State* L)
    {
        const std::string command = luaL_checkstring(L, 1);
        long timeout              = 10;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            timeout = std::clamp<long>(luaL_checkinteger(L, 2), 1, 120);
        }

        std::string workspace;
        if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
            const std::string raw = luaL_checkstring(L, 3);
            if (raw.empty()) {
                return binding_error(L, "shell: workspace must not be empty");
            }
            fs::path dir(raw);
            if (dir.is_relative()) {
                // The session working directory lives in the workspace
                // snapshot; the process cwd already tracks it (Environment
                // chdirs the process), so this is plain path joining.
                LuaRunContext* run              = run_of(L);
                const PermissionContext context = run->host->permission_context
                    ? run->host->permission_context()
                    : PermissionContext { };
                dir = (context.workspace ? context.workspace->working_directory
                                         : fs::current_path())
                    / dir;
            }
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                return binding_error(
                    L, "shell: workspace is not a directory: " + raw);
            }
            workspace = dir.string();
        }

        const ShellAnalysis analysis = analyze_shell(command);
        if (analysis.invocations.size() > 1) {
            return binding_error(L,
                "shell: one command per call; compose results in Lua instead "
                "of "
                "chaining with && || ; |");
        }
        if (analysis.invocations.empty()) {
            return binding_error(L, "shell: empty command");
        }

        const GateOutcome gate = authorize_shell(L,
            ShellRequest {
                command, std::chrono::seconds(timeout), fs::path(workspace) });
        if (!gate) {
            return binding_error(L, gate.denial);
        }
        const std::string run_dir
            = gate.shell ? gate.shell->workspace.string() : workspace;

        CommandResult r = run_command(
            command, std::chrono::seconds(timeout), fs::path(run_dir));
        if (!r.spawned) {
            record_call(L, "shell", command, false);
            return binding_error(L, "shell: failed to execute command");
        }
        if (r.timed_out) {
            record_call(L, "shell", command, false);
            return binding_error(
                L, "shell: timed out after " + std::to_string(timeout) + "s");
        }
        record_call(L, "shell", command, r.exit_code == 0);
        std::string output = std::move(r.output);
        if (output.size() > MAX_OUTPUT_BYTES) {
            output.resize(MAX_OUTPUT_BYTES);
            output += "\n[truncated]";
        }
        lua_pushlstring(L, output.data(), output.size());
        lua_pushinteger(L, r.exit_code);
        return 2;
    }

    constexpr LuaBinding BINDINGS[] = {
        {
            "shell",
            binding_shell,
            R"desc(tool.shell(command: string, timeout?: integer=10, workspace?: string) => output: string, exit_code: integer
Runs a single external command, returning its captured output (capped at 64 KB)
and exit status.
A non-zero exit_code is a successful call, so test exit_code rather than nil.
nil, Err means it could not start, timed out (1..120 s) or was denied.
Chains and pipelines are rejected. Compose results in Lua instead.
`workspace` is the directory the command runs in.)desc",
            LuaCapability::SHELL,
            "shell: shell access is disabled for this run",
        },
    };

} // namespace

std::span<const LuaBinding> shell_lua_bindings() { return BINDINGS; }

} // namespace imza
