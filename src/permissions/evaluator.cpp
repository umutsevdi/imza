#include "permissions/evaluator.h"

#include "network/json_io.h"
#include "permissions/shell_analysis.h"
#include "platform/config.h"
#include "tools/skills.h"
#include "tools/tool.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace imza {

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

    ShellEvaluation evaluate_shell(
        ShellRequest request, const PermissionContext& context)
    {
        if (request.command.empty()) {
            return { { PermissionDecision::Kind::REJECT,
                         "shell: 'command' must be a non-empty string" },
                std::move(request), { } };
        }
        if (!context.grants) {
            return { { PermissionDecision::Kind::REJECT,
                         "permission grants are unavailable" },
                std::move(request), { } };
        }
        const ShellAnalysis analysis = analyze_shell(request.command);
        if (analysis.reuse == ShellAnalysis::Reuse::ONCE) {
            return { { PermissionDecision::Kind::ASK,
                         "shell syntax requires approval for each execution" },
                std::move(request), { } };
        }

        PermissionStore::Grants candidates;
        for (const ShellInvocation& invocation : analysis.invocations) {
            if (shell_builtin_allowed(invocation.program)
                || shell_readonly_allowed(invocation)) {
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
            return { { PermissionDecision::Kind::ACCEPT, "" },
                std::move(request), { } };
        }
        const std::string reason = shell_grant_reason(candidates);
        return { { PermissionDecision::Kind::ASK, reason }, std::move(request),
            std::move(candidates) };
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

    if (original.name == "skill") {
        return evaluate_skill(original, context, config, skills, loaded_skills);
    }

    const Json::Value arguments = parse_json(original.args);
    if (original.name == "subagent") {
        if (const auto error = validate_subagent_tool_arguments(
                arguments, context.mode == Session::Mode::BUILD)) {
            return reject(std::move(request), *error);
        }
        return accept(std::move(request));
    }
    if (original.name == "lua") {
        if (!arguments["script"].isString()
            || arguments["script"].asString().empty()) {
            return reject(std::move(request),
                "lua: expected a non-empty 'script' string");
        }
        return accept(std::move(request));
    }
    return reject(
        std::move(request), "tool has no permission policy: " + original.name);
}

ShellEvaluation evaluate_shell_request(
    ShellRequest request, const PermissionContext& context)
{
    return evaluate_shell(std::move(request), context);
}

} // namespace imza
