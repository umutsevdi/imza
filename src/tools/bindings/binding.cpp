#include "tools/bindings.h"

#include "permissions/filesystem.h"

#include <chrono>
#include <type_traits>

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

namespace {

    PermissionPrompt filesystem_prompt(const FilesystemRequest& request,
        std::string reason, bool allow_for_session)
    {
        PermissionPrompt prompt;
        prompt.name        = std::string(filesystem_request_name(request));
        prompt.description = prompt.name;
        prompt.reason      = std::move(reason);
        prompt.target      = filesystem_target(request).string();
        prompt.allow_for_session = allow_for_session;
        std::visit(
            [&](const auto& operation) {
                using T = std::decay_t<decltype(operation)>;
                if constexpr (std::is_same_v<T, ReadFileRequest>) {
                    prompt.first_line = operation.first_line;
                    prompt.last_line  = operation.last_line;
                } else if constexpr (std::is_same_v<T, InsertFileRequest>) {
                    prompt.text = operation.text;
                    prompt.line = operation.line;
                } else if constexpr (std::is_same_v<T, WriteFileRequest>) {
                    prompt.text = operation.text;
                } else if constexpr (std::is_same_v<T, EditFileRequest>) {
                    prompt.old_text = operation.old_text;
                    prompt.new_text = operation.new_text;
                }
            },
            request);
        return prompt;
    }

} // namespace

std::optional<FilesystemRequest> authorize_filesystem(
    lua_State* L, FilesystemRequest request, std::string_view label_override)
{
    LuaRunContext* run      = run_of(L);
    const std::string label = label_override.empty()
        ? std::string(filesystem_request_name(request))
        : std::string(label_override);
    const auto record       = [&](bool ok, const FilesystemRequest& allowed) {
        run->log.push_back({ label, filesystem_target(allowed).string(), ok });
    };
    if (!run->host->permission_context) {
        // No provider: trusted mode (tests, SKIP_PERMISSIONS paths).
        record(true, request);
        return request;
    }
    const FilesystemEvaluation evaluation
        = evaluate_filesystem_request(request, run->host->permission_context());
    switch (evaluation.decision.kind) {
    case PermissionDecision::Kind::ACCEPT:
        record(true, *evaluation.request);
        return evaluation.request;
    case PermissionDecision::Kind::REJECT: return std::nullopt;
    case PermissionDecision::Kind::ASK: break;
    }
    if (run->host->skip_permissions) {
        record(true, *evaluation.request);
        return evaluation.request;
    }
    if (run->host->unattended) {
        run->blocked_permission = true;
        return std::nullopt;
    }
    if (!run->host->ask) {
        return std::nullopt;
    }
    PermissionStore::Grants grants;
    if (const auto grant = filesystem_session_grant(*evaluation.request)) {
        grants.push_back(PermissionGrant { *grant });
    }
    PermissionPrompt prompt = filesystem_prompt(
        *evaluation.request, evaluation.decision.reason, !grants.empty());
    // Human think-time is free: shift the wall-clock deadline by the paused
    // duration so a slow approval does not consume the script's budget.
    const auto paused_at     = std::chrono::steady_clock::now();
    const ModalResult result = run->host->ask(std::move(prompt)).get();
    run->deadline += std::chrono::steady_clock::now() - paused_at;
    const auto* verdict = std::get_if<ToolVerdict>(&result);
    if (verdict == nullptr || verdict->decision == ToolDecision::REJECT) {
        return std::nullopt;
    }
    if (verdict->decision == ToolDecision::ACCEPT_FOR_SESSION) {
        if (grants.empty() || !run->host->install_grants
            || !run->host->install_grants(std::move(grants))) {
            return std::nullopt;
        }
        const FilesystemEvaluation current = evaluate_filesystem_request(
            request, run->host->permission_context());
        if (current.decision.kind != PermissionDecision::Kind::ACCEPT
            || !current.request || *current.request != *evaluation.request) {
            return std::nullopt;
        }
        record(true, *current.request);
        return current.request;
    }
    record(true, *evaluation.request);
    return evaluation.request;
}

} // namespace imza
