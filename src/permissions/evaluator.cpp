#include "permissions/evaluator.h"

#include "conversation/session.h"

#include "network/json.h"
#include "permissions/shell_analysis.h"
#include "platform/config.h"
#include "runtime/mcp_subsystem.h"
#include "tools/skills.h"
#include "tools/tool.h"

#include <filesystem>
#include <string>
#include <utility>

namespace imza {

namespace {

    PermissionEvaluation reject(ToolCallRequest request, std::string reason)
    {
        return { { PermissionDecision::Kind::REJECT, std::move(reason) },
            std::move(request), { }, std::nullopt };
    }

    PermissionEvaluation accept(ToolCallRequest request)
    {
        return { { PermissionDecision::Kind::ACCEPT, "" }, std::move(request),
            { }, std::nullopt };
    }

    PermissionEvaluation ask(ToolCallRequest request, std::string reason,
        PermissionStore::Grants grants, SkillRequest details)
    {
        PermissionPrompt prompt;
        prompt.name              = "skill";
        prompt.description       = request.description;
        prompt.reason            = reason;
        prompt.allow_for_session = !grants.empty();
        prompt.id                = request.id;
        prompt.request           = std::move(details);
        return { { PermissionDecision::Kind::ASK, std::move(reason) },
            std::move(request), std::move(grants), std::move(prompt) };
    }

    PermissionStore::Grants grants_of(const PermissionContext& context)
    {
        return context.grants ? *context.grants : PermissionStore::Grants { };
    }

    // every disconnected load asks again, and only --allow-mcp spares it.
    PermissionEvaluation evaluate_load_mcp(ToolCallRequest request,
        const std::string& server, const PermissionContext& context,
        PermissionStore::Grants grants)
    {
        const McpSubsystem* manager = context.mcp;
        std::optional<McpServerSnapshot> found;
        if (manager != nullptr) {
            for (const McpServerSnapshot& candidate : manager->snapshot()) {
                if (candidate.id == server) {
                    found = candidate;
                    break;
                }
            }
        }
        // Nothing to approve: the handler reports unknown servers.
        if (!found) {
            return accept(std::move(request));
        }
        if (grants_cover(grants, McpGrant { server })) {
            return accept(std::move(request));
        }
        switch (found->state) {
        case McpServerState::CONNECTED:
        case McpServerState::CONNECTING: return accept(std::move(request));
        case McpServerState::INACTIVE:
            return reject(std::move(request),
                "load_mcp: server '" + server
                    + "' is disabled; enable it with /mcp");
        case McpServerState::FAILED:
            return reject(std::move(request),
                "load_mcp: server '" + server + "' failed: "
                    + (found->detail.empty() ? std::string("connection error")
                                             : found->detail));
        case McpServerState::OFFLINE: break;
        }
        PermissionPrompt prompt;
        prompt.name        = "load_mcp";
        prompt.description = "Connect and load MCP server '" + server + "'";
        prompt.reason      = "connecting to an MCP server requires approval";
        prompt.target      = found->label != server ? found->label : "";
        prompt.allow_for_session = false;
        prompt.id                = request.id;
        prompt.request = McpRequest { server, found->label, found->detail };
        return { { PermissionDecision::Kind::ASK, prompt.reason },
            std::move(request), { }, std::move(prompt) };
    }

    PermissionEvaluation evaluate_skill(const ToolCallRequest& original,
        const PermissionContext& context, const Config& config,
        const std::vector<Skill>& skills, const Session& session)
    {
        SkillToolArgs arguments;
        if (json_parse_checked(original.args, arguments)) {
            return reject(original, "skill: unknown or unavailable skill");
        }
        const std::optional<Skill> skill = resolve_skill(skills, arguments);
        if (!skill) {
            return reject(original, "skill: unknown or unavailable skill");
        }
        if (skill_policy(config, *skill) == SkillPolicy::DENY) {
            return reject(original, "skill: access denied by configuration");
        }
        const std::optional<std::filesystem::path> path
            = canonical_skill_path(*skill);
        if (!path) {
            return reject(original, "skill: cannot normalize instructions");
        }
        ToolCallRequest request = original;
        SkillToolArgs normalized;
        normalized.name = skill->name;
        normalized.scope
            = skill->scope == Skill::Scope::PROJECT ? "project" : "global";
        normalized.path = path->string();
        request.args    = json_dump(normalized);

        const SkillGrant grant { *path };
        if (session.is_skill_loaded(skill->path)
            || session.is_skill_loaded(*path)
            || grants_cover(context.grants, grant)) {
            return accept(std::move(request));
        }
        std::string reason;
        if (!load_skill_checked(*skill, reason)) {
            return reject(original, "skill: " + reason);
        }
        if (skill_policy(config, *skill) == SkillPolicy::ALLOW) {
            return accept(std::move(request));
        }
        SkillRequest details;
        details.name = skill->name;
        details.scope
            = skill->scope == Skill::Scope::PROJECT ? "project" : "global";
        details.path = path->string();
        return ask(std::move(request), "skill instructions require approval",
            { PermissionGrant { grant } }, std::move(details));
    }

} // namespace

PermissionEvaluation evaluate_tool_request(const ToolCallRequest& original,
    const PermissionContext& context, const Config& config,
    const std::vector<Skill>& skills, const Session& session)
{
    ToolCallRequest request = original;

    const std::optional<RosterTool> tool = classify_roster_tool(original.name);
    if (!tool) {
        return reject(std::move(request),
            "tool has no permission policy: " + original.name);
    }

    switch (*tool) {
    case RosterTool::SKILL:
        return evaluate_skill(original, context, config, skills, session);
    case RosterTool::SUBAGENT: {
        SubagentToolArgs arguments;
        if (json_parse_checked(original.args, arguments)) {
            return reject(std::move(request), std::string(SUBAGENT_ARGS_ERROR));
        }
        if (const auto error = validate_subagent_tool_arguments(
                arguments, context.mode == SessionMode::BUILD)) {
            return reject(std::move(request), *error);
        }
        return accept(std::move(request));
    }
    case RosterTool::LUA: {
        LuaToolArgs arguments;
        if (json_parse_checked(original.args, arguments)
            || arguments.script.empty()) {
            return reject(std::move(request), std::string(LUA_ARGS_ERROR));
        }
        return accept(std::move(request));
    }
    case RosterTool::LOAD: {
        LoadToolArgs arguments;
        if (json_parse_checked(original.args, arguments)
            || arguments.name.empty()) {
            return reject(std::move(request), std::string(LOAD_ARGS_ERROR));
        }
        return accept(std::move(request));
    }
    case RosterTool::LOAD_MCP: {
        LoadMcpToolArgs arguments;
        if (json_parse_checked(original.args, arguments)
            || arguments.server.empty()) {
            return reject(std::move(request), std::string(LOAD_MCP_ARGS_ERROR));
        }
        return evaluate_load_mcp(std::move(request), arguments.server, context,
            std::move(grants_of(context)));
    }
    }
    return reject(
        std::move(request), "tool has no permission policy: " + original.name);
}

std::optional<RosterTool> classify_roster_tool(std::string_view name)
{
    if (name == "skill") {
        return RosterTool::SKILL;
    }
    if (name == "subagent") {
        return RosterTool::SUBAGENT;
    }
    if (name == "lua") {
        return RosterTool::LUA;
    }
    if (name == "load") {
        return RosterTool::LOAD;
    }
    if (name == "load_mcp") {
        return RosterTool::LOAD_MCP;
    }
    return std::nullopt;
}

} // namespace imza
