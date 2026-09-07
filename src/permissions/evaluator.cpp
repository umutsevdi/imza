#include "permissions/evaluator.h"

#include "network/json_io.h"
#include "platform/config.h"
#include "tools/skills.h"
#include "tools/tool.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace ursa {

namespace {

    PermissionEvaluation reject(ToolCallRequest request, std::string reason)
    {
        request.permission_reason = reason;
        return { { PermissionDecision::Kind::REJECT, std::move(reason) },
            std::move(request), { } };
    }

    PermissionEvaluation accept(ToolCallRequest request)
    {
        request.permission_reason.clear();
        request.allow_for_session = false;
        return { { PermissionDecision::Kind::ACCEPT, "" }, std::move(request),
            { } };
    }

    PermissionEvaluation ask(ToolCallRequest request, std::string reason,
        PermissionStore::Grants grants = { })
    {
        request.permission_reason = reason;
        request.allow_for_session = !grants.empty();
        return { { PermissionDecision::Kind::ASK, std::move(reason) },
            std::move(request), std::move(grants) };
    }

    bool matches_skill(
        const PermissionStore::Grants& grants, const SkillGrant& requested)
    {
        return std::any_of(
            grants.begin(), grants.end(), [&](const auto& grant) {
                const auto* skill = std::get_if<SkillGrant>(&grant);
                return skill != nullptr && *skill == requested;
            });
    }

    bool matches_shell(const PermissionStore::Grants& grants,
        const ShellCommandGrant& requested)
    {
        return std::any_of(
            grants.begin(), grants.end(), [&](const auto& grant) {
                const auto* shell = std::get_if<ShellCommandGrant>(&grant);
                return shell != nullptr && shell->program == requested.program
                    && (!shell->subcommand
                        || shell->subcommand == requested.subcommand);
            });
    }

    bool has_shell_program(
        const PermissionStore::Grants& grants, std::string_view program)
    {
        return std::any_of(
            grants.begin(), grants.end(), [&](const auto& grant) {
                const auto* shell = std::get_if<ShellCommandGrant>(&grant);
                return shell != nullptr && shell->program == program;
            });
    }

    std::string shell_grant_reason(const PermissionStore::Grants& grants)
    {
        std::string reason
            = "shell commands require approval · session scope: ";
        for (const PermissionGrant& grant : grants) {
            const auto& shell = std::get<ShellCommandGrant>(grant);
            if (!reason.ends_with(": ")) {
                reason += ", ";
            }
            reason += shell.program + " " + shell.subcommand.value_or("*");
        }
        return reason;
    }

    PermissionEvaluation evaluate_shell(const ToolCallRequest& original,
        const Json::Value& arguments, const PermissionContext& context)
    {
        if (!context.grants) {
            return reject(original, "permission grants are unavailable");
        }
        const ShellAnalysis analysis
            = analyze_shell(arguments["command"].asString());
        if (analysis.reuse == ShellAnalysis::Reuse::ONCE) {
            return ask(
                original, "shell syntax requires approval for each execution");
        }

        PermissionStore::Grants candidates;
        for (const ShellInvocation& invocation : analysis.invocations) {
            if (shell_builtin_allowed(invocation.program)) {
                continue;
            }
            const ShellCommandGrant requested { invocation.program,
                invocation.subcommand };
            if (matches_shell(*context.grants, requested)) {
                continue;
            }
            ShellCommandGrant candidate = requested;
            if (has_shell_program(*context.grants, requested.program)) {
                candidate.subcommand.reset();
            }
            for (PermissionGrant& pending : candidates) {
                auto* shell = std::get_if<ShellCommandGrant>(&pending);
                if (shell == nullptr || shell->program != candidate.program) {
                    continue;
                }
                if (shell->subcommand != candidate.subcommand) {
                    shell->subcommand.reset();
                }
                candidate.program.clear();
                break;
            }
            if (!candidate.program.empty()) {
                candidates.push_back(PermissionGrant { std::move(candidate) });
            }
        }
        if (candidates.empty()) {
            return accept(original);
        }
        const std::string reason = shell_grant_reason(candidates);
        return ask(original, reason, std::move(candidates));
    }

    PermissionEvaluation evaluate_skill(const ToolCallRequest& original,
        const PermissionContext& context, const Config& config,
        const std::vector<Skill>& skills, const SkillStore& loaded_skills)
    {
        Json::Value arguments            = parse_json(original.args);
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
        Json::Value normalized(Json::objectValue);
        normalized["name"] = skill->name;
        normalized["scope"]
            = skill->scope == Skill::Scope::PROJECT ? "project" : "global";
        normalized["path"] = path->string();
        request.args       = write_json(normalized);

        const SkillGrant grant { *path };
        if (loaded_skills.is_loaded(skill->path)
            || loaded_skills.is_loaded(*path)
            || (context.grants && matches_skill(*context.grants, grant))) {
            return accept(std::move(request));
        }
        const SkillRead read = read_skill(*skill);
        if (read.kind == SkillRead::Kind::READ_FAILED) {
            return reject(original, "skill: cannot read instructions");
        }
        if (read.kind == SkillRead::Kind::TOO_LARGE) {
            return reject(original, "skill: instructions exceed 128 KiB");
        }
        if (skill_policy(config, *skill) == SkillPolicy::ALLOW) {
            return accept(std::move(request));
        }
        return ask(std::move(request), "skill instructions require approval",
            { PermissionGrant { grant } });
    }

} // namespace

PermissionEvaluation evaluate_tool_request(const ToolCallRequest& original,
    const PermissionContext& context, const Config& config,
    const std::vector<Skill>& skills, const SkillStore& loaded_skills)
{
    ToolCallRequest request = original;
    request.permission_reason.clear();
    request.allow_for_session = false;

    if (original.name == "read" || original.name == "list"
        || original.name == "edit" || original.name == "write") {
        const FilesystemEvaluation filesystem = evaluate_filesystem_request(
            original.name, original.args, context);
        if (!filesystem.request) {
            return reject(std::move(request), filesystem.decision.reason);
        }
        request.args = write_json(filesystem.request->normalized_arguments);
        if (filesystem.decision.kind == PermissionDecision::Kind::ACCEPT) {
            return accept(std::move(request));
        }
        if (filesystem.decision.kind == PermissionDecision::Kind::REJECT) {
            return reject(std::move(request), filesystem.decision.reason);
        }
        PermissionStore::Grants grants;
        if (const auto grant = filesystem_session_grant(*filesystem.request)) {
            grants.push_back(PermissionGrant { *grant });
        }
        return ask(
            std::move(request), filesystem.decision.reason, std::move(grants));
    }
    if (original.name == "skill") {
        return evaluate_skill(original, context, config, skills, loaded_skills);
    }

    const Json::Value arguments = parse_json(original.args);
    if (original.name == "ask") {
        if (!parse_ask_args(original.args)) {
            return reject(std::move(request),
                "ask: expected a non-empty 'questions' array");
        }
        return accept(std::move(request));
    }
    if (original.name == "todo") {
        if (!parse_todo_args(arguments)) {
            return reject(std::move(request),
                "todo: expected a 'todos' array of {content, status} objects");
        }
        return accept(std::move(request));
    }
    if (original.name == "subagent") {
        if (const auto error = validate_subagent_tool_arguments(
                arguments, context.mode == Session::Mode::BUILD)) {
            return reject(std::move(request), *error);
        }
        return accept(std::move(request));
    }
    if (original.name == "webfetch" || original.name == "websearch") {
        if (const auto error
            = validate_web_tool_arguments(original.name, arguments)) {
            return reject(std::move(request), *error);
        }
        return accept(std::move(request));
    }
    if (original.name == "shell") {
        if (const auto error = validate_shell_tool_arguments(arguments)) {
            return reject(std::move(request), *error);
        }
        return evaluate_shell(request, arguments, context);
    }
    return reject(
        std::move(request), "tool has no permission policy: " + original.name);
}

} // namespace ursa
