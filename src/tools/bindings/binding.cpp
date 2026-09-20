#include "tools/bindings.h"

#include "permissions/filesystem.h"
#include "permissions/shell.h"

#include <chrono>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {

LuaRunContext* run_of(lua_State* L)
{
    return *static_cast<LuaRunContext**>(lua_getextraspace(L));
}

void record_call(
    lua_State* L, std::string_view binding, std::string target, bool ok)
{
    run_of(L)->log.push_back({ std::string(binding), std::move(target), ok });
}

int binding_error(lua_State* L, std::string message)
{
    lua_pushnil(L);
    lua_pushlstring(L, message.data(), message.size());
    return 2;
}

std::string gate_denied(
    lua_State* L, const std::string& denial, const std::string& path)
{
    std::string text(run_of(L)->current_binding);
    text += ": permission denied: " + path;
    if (!denial.empty()) {
        text += " (" + denial + ")";
    }
    return text;
}

ModalResult ask_with_deadline_credit(LuaRunContext& run, ModalPayload payload)
{
    // Human think-time is free: shift the wall-clock deadline by the
    // paused duration so a slow approval does not consume the script's
    // execution budget.
    const auto paused_at     = std::chrono::steady_clock::now();
    const ModalResult result = run.host->ask(std::move(payload)).get();
    run.deadline += std::chrono::steady_clock::now() - paused_at;
    return result;
}

namespace {

    PermissionPrompt gate_prompt(std::string name, std::string reason,
        std::string target, bool allow_for_session,
        std::variant<std::monostate, FilesystemRequest, ShellRequest> request)
    {
        PermissionPrompt prompt;
        prompt.name              = std::move(name);
        prompt.description       = prompt.name;
        prompt.reason            = std::move(reason);
        prompt.target            = std::move(target);
        prompt.allow_for_session = allow_for_session;
        prompt.request           = std::move(request);
        return prompt;
    }

} // namespace

bool resolve_ask(LuaRunContext& run, const std::string& label,
    const std::string& target, PermissionStore::Grants session_grants,
    PermissionPrompt prompt, const std::function<bool()>& recheck,
    std::string& denial)
{
    const auto deny = [&](std::string reason) {
        run.log.push_back({ label, target, false });
        denial = std::move(reason);
        return false;
    };
    if (run.host->skip_permissions) {
        return true;
    }
    if (run.host->unattended) {
        run.blocked_permission = true;
        return deny(label + ": permission requires approval in attended runs");
    }
    if (!run.host->ask) {
        return deny(label + ": permission approval unavailable");
    }
    const ModalResult result = ask_with_deadline_credit(run, std::move(prompt));
    const auto* verdict      = std::get_if<ToolVerdict>(&result);
    if (verdict == nullptr) {
        return deny(label + ": permission dismissed");
    }
    if (verdict->decision == ToolDecision::REJECT) {
        return deny(label + ": "
            + (verdict->reason.empty() ? "rejected" : verdict->reason));
    }
    if (verdict->decision == ToolDecision::ACCEPT_FOR_SESSION) {
        if (session_grants.empty() || !run.host->install_grants
            || !run.host->install_grants(std::move(session_grants))) {
            return deny(label + ": session approval is unavailable");
        }
        // The installed grant may turn the request into an auto-accept;
        // execution only continues on the re-evaluated verdict.
        if (!recheck()) {
            return deny(label + ": permission target changed before execution");
        }
    }
    return true;
}

GateOutcome authorize_filesystem(lua_State* L, FilesystemRequest request)
{
    GateOutcome outcome;
    LuaRunContext* run = run_of(L);
    const std::string label(run->current_binding);
    const auto allow = [&](const FilesystemRequest& allowed) {
        run->log.push_back(
            { label, filesystem_target(allowed).string(), true });
        outcome.filesystem = allowed;
        return outcome;
    };
    if (!run->host->permission_context) {
        // No provider: trusted mode (tests, SKIP_PERMISSIONS paths).
        return allow(request);
    }
    const FilesystemEvaluation evaluation
        = evaluate_filesystem_request(request, run->host->permission_context());
    if (!evaluation.request) {
        run->log.push_back(
            { label, filesystem_target(request).string(), false });
        outcome.denial = label + ": " + evaluation.decision.reason;
        return outcome;
    }
    if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT) {
        return allow(*evaluation.request);
    }
    PermissionStore::Grants grants;
    if (const auto grant = filesystem_session_grant(*evaluation.request)) {
        grants.push_back(PermissionGrant { *grant });
    }
    const std::string target = filesystem_target(*evaluation.request).string();
    const bool allowed       = resolve_ask(
        *run, label, target, grants,
        // The modal keys off the gate operation name ("edit", "write");
        // the dispatch log records the binding's catalog path.
        gate_prompt(std::string(filesystem_request_name(*evaluation.request)),
            evaluation.decision.reason, target, !grants.empty(),
            *evaluation.request),
        [&] {
            const FilesystemEvaluation current = evaluate_filesystem_request(
                request, run->host->permission_context());
            return current.decision.kind == PermissionDecision::Kind::ACCEPT
                && current.request && *current.request == *evaluation.request;
        },
        outcome.denial);
    return allowed ? allow(*evaluation.request) : outcome;
}

GateOutcome authorize_shell(lua_State* L, const ShellRequest& request)
{
    GateOutcome outcome;
    LuaRunContext* run = run_of(L);
    const std::string label(run->current_binding);
    // No allow-path log entry: the binding records the execution outcome
    // (exit status / failure), which is what the dispatch log shows for
    // shell. resolve_ask logs denials.
    const auto allow = [&] {
        outcome.shell = request;
        return outcome;
    };
    if (!run->host->permission_context) {
        // No provider: trusted mode (tests, SKIP_PERMISSIONS paths).
        return allow();
    }
    const ShellEvaluation evaluation
        = evaluate_shell_request(request, run->host->permission_context());
    if (evaluation.decision.kind == PermissionDecision::Kind::REJECT) {
        run->log.push_back({ label, request.command, false });
        outcome.denial = label + ": " + evaluation.decision.reason;
        return outcome;
    }
    if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT) {
        return allow();
    }
    const bool allowed = resolve_ask(
        *run, label, request.command, evaluation.session_grants,
        gate_prompt(label, evaluation.decision.reason,
            evaluation.request.workspace.string(),
            !evaluation.session_grants.empty(), evaluation.request),
        [&] {
            const ShellEvaluation current = evaluate_shell_request(
                request, run->host->permission_context());
            return current.decision.kind != PermissionDecision::Kind::REJECT
                && current.request == evaluation.request;
        },
        outcome.denial);
    return allowed ? allow() : outcome;
}

} // namespace imza
