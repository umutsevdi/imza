#include "tools/bindings.h"

#include "permissions/evaluator.h"
#include "permissions/shell_analysis.h"
#include "platform/command_runner.h"
#include "workspace/environment.h"

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

    int tool_sh(lua_State* L)
    {
        const std::string command = luaL_checkstring(L, 1);
        long timeout              = 10;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            timeout = std::clamp<long>(luaL_checkinteger(L, 2), 1, 120);
        }

        LuaRunContext* run = run_of(L);
        if (run->host == nullptr || !run->host->shell_enabled) {
            return binding_error(
                L, "shell: shell access is disabled for this run");
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
                const PermissionContext context = run->host->context
                    ? run->host->context()
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

        const auto run_sh = [&]() -> int {
            CommandResult r = run_command(
                command, std::chrono::seconds(timeout), fs::path(workspace));
            if (!r.spawned) {
                record_call(L, "shell", command, false);
                return binding_error(L, "shell: failed to execute command");
            }
            if (r.timed_out) {
                record_call(L, "shell", command, false);
                return binding_error(L,
                    "shell: timed out after " + std::to_string(timeout) + "s");
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
        };

        if (run->host == nullptr || !run->host->context) {
            // No provider: trusted mode (tests, SKIP_PERMISSIONS paths).
            return run_sh();
        }

        const ShellRequest request { command, std::chrono::seconds(timeout),
            fs::path(workspace) };
        const ShellEvaluation evaluation
            = evaluate_shell_request(request, run->host->context());
        if (evaluation.decision.kind == PermissionDecision::Kind::REJECT) {
            return binding_error(L, "shell: " + evaluation.decision.reason);
        }
        if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT
            || run->host->skip_permissions) {
            return run_sh();
        }
        if (run->host->unattended) {
            run->blocked_permission = true;
            return binding_error(
                L, "shell: permission requires approval in attended runs");
        }
        if (!run->host->ask) {
            return binding_error(L, "shell: permission approval unavailable");
        }

        // ASK: block on the modal queue; think-time is free, so shift the
        // wall-clock deadline by the paused duration.
        PermissionPrompt prompt;
        prompt.name              = "shell";
        prompt.description       = "shell";
        prompt.reason            = evaluation.decision.reason;
        prompt.command           = evaluation.request.command;
        prompt.target            = evaluation.request.workspace.string();
        prompt.timeout           = evaluation.request.timeout;
        prompt.allow_for_session = !evaluation.session_grants.empty();
        const auto paused_at     = std::chrono::steady_clock::now();
        const ModalResult result = run->host->ask(std::move(prompt)).get();
        run->deadline += std::chrono::steady_clock::now() - paused_at;
        const auto* verdict = std::get_if<ToolVerdict>(&result);
        if (verdict == nullptr) {
            return binding_error(L, "shell: permission dismissed");
        }
        if (verdict->decision == ToolDecision::REJECT) {
            return binding_error(L,
                "shell: "
                    + (verdict->reason.empty() ? "rejected" : verdict->reason));
        }
        if (verdict->decision == ToolDecision::ACCEPT_FOR_SESSION) {
            if (evaluation.session_grants.empty() || !run->host->install_grants
                || !run->host->install_grants(evaluation.session_grants)) {
                return binding_error(
                    L, "shell: session approval is unavailable");
            }
        }

        // Re-evaluate before execution: the grant above may have turned the
        // request into an auto-accept.
        const ShellEvaluation current
            = evaluate_shell_request(request, run->host->context());
        if (current.decision.kind == PermissionDecision::Kind::REJECT
            || current.request != evaluation.request) {
            return binding_error(L, "shell: " + current.decision.reason);
        }
        return run_sh();
    }

    constexpr LuaBinding BINDINGS[] = {
        {
            "shell",
            tool_sh,
            "tool.shell(command: string, timeout?: integer=10, "
            "workspace?: string)\n    => output: string, exit_code: integer",
            "Runs a single external command, returning its captured output "
            "(capped at\n"
            "64 KB) and exit status.\n"
            "A non-zero exit_code is a successful call, so test exit_code "
            "rather than nil.\n"
            "nil, Err means it could not start, timed out (1..120 s) or was "
            "denied.\n"
            "Chains and pipelines are rejected. Compose results in Lua "
            "instead.\n"
            "`workspace` is the directory the command runs in.",
            LuaCapability::SHELL,
        },
    };

} // namespace

std::span<const LuaBinding> shell_lua_bindings() { return BINDINGS; }

} // namespace imza
