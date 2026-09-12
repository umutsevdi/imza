#include "turn/prompt.h"
#include "app/application_state.h"
#include "common/util.h"
#include "conversation/session.h"
#include "tools/skills.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace imza {

namespace {

    std::string environment_block(
        const SystemEnvironment& sys, const WorkspaceEnvironment* ws)
    {
        std::string out = "<env>";
        out += "\n  Current Directory: ";
        out += ws == nullptr ? "unknown" : ws->working_directory.string();
        if (ws != nullptr && ws->project_root.has_value()) {
            out += "\n  Project Root: " + ws->project_root.value().string();
        }
        out += "\n  Operating System: ";
        out += sys.os_name;
        if (!sys.os_version.empty()) {
            out += " ";
            out += sys.os_version;
        }
        out += "\n  Shell: ";
        out += sys.default_shell;
        out += "\n  Package managers: ";
        out += sys.package_managers.empty() ? std::string("none")
                                            : join(sys.package_managers, ", ");
        out += "\n  Today's date: ";
        out += sys.today;
        out += "\n</env>";
        return out;
    }

    std::string instructions_block(const InstructionFile& file)
    {
        std::string out = "<instructions source=\"";
        out += file.path;
        out += "\">\n";
        out += file.content;
        if (out.back() != '\n') {
            out += '\n';
        }
        out += "</instructions>";
        return out;
    }

    void append_context(std::string& out, const SystemEnvironment* sys,
        const WorkspaceEnvironment* ws, const Config* config)
    {
        if (sys == nullptr) {
            return;
        }
        out += "\n\n";
        out += environment_block(*sys, ws);
        if (ws != nullptr && ws->instruction) {
            out += "\n\n";
            out += instructions_block(*ws->instruction);
        }
        std::vector<Skill> skills;
        for (const auto& [name, skill] : sys->global_skills) {
            skills.push_back(skill);
        }
        if (ws != nullptr) {
            for (const auto& [name, skill] : ws->project_skills) {
                skills.push_back(skill);
            }
        }
        std::sort(
            skills.begin(), skills.end(), [](const Skill& a, const Skill& b) {
                if (a.scope != b.scope) {
                    return a.scope == Skill::Scope::PROJECT;
                }
                return a.name < b.name;
            });
        std::string catalog;
        for (const Skill& skill : skills) {
            SkillPolicy policy = SkillPolicy::ASK;
            if (config != nullptr) {
                if (skill.scope == Skill::Scope::GLOBAL) {
                    if (auto it = config->global_skills.find(skill.name);
                        it != config->global_skills.end()) {
                        policy = it->second;
                    }
                } else if (skill.project_root) {
                    auto project = config->project_skills.find(
                        skill.project_root->string());
                    if (project != config->project_skills.end()) {
                        if (auto it = project->second.find(skill.name);
                            it != project->second.end()) {
                            policy = it->second;
                        }
                    }
                }
            }
            if (policy == SkillPolicy::DENY) {
                continue;
            }
            catalog += "- ";
            catalog += skill.name + " ["
                + (skill.scope == Skill::Scope::PROJECT ? "project" : "global")
                + "]";
            if (!skill.description.empty()) {
                catalog += ": " + skill.description + "\n";
            }
        }
        if (!catalog.empty()) {
            out += "\n\n<skills>\n";
            out += catalog;
            out += "\n</skills>";
        }
    }

    std::string mode_reminder(
        std::string_view tag, std::string_view instructions)
    {
        std::string out(tag);
        out += '\n';
        out += instructions;
        out += "\n</system-reminder>";
        return out;
    }

} // namespace

std::string build_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    const Config* config)
{
    std::string out = prompts.system();
    append_context(out, sys, ws, config);
    return out;
}

std::string build_subagent_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    SubagentRole role, const Config* config)
{
    if (role == SubagentRole::BASIC) {
        return { };
    }
    std::string out = prompts.subagent();
    out += "\n\n";
    out += role == SubagentRole::RESEARCH ? prompts.subagent_research()
                                          : prompts.subagent_build();
    append_context(out, sys, ws, config);
    return out;
}

std::string title_prompt(const PromptStore& prompts, std::string_view request)
{
    std::string out = prompts.title();
    out += "\n\nUser request:\n";
    out += request;
    return out;
}

std::string plan_mode_reminder(const PromptStore& prompts)
{
    return mode_reminder(PLAN_REMINDER_TAG, prompts.reminder_plan());
}

std::string build_mode_reminder(const PromptStore& prompts)
{
    return mode_reminder(BUILD_REMINDER_TAG, prompts.reminder_build());
}

std::string full_system_prompt(const ApplicationState& state)
{
    const std::shared_ptr<Environment> env = state.environment;
    const Config config                    = state.providers->config();
    std::string prompt                     = build_system_prompt(
        *state.prompts, env->system().get(), env->workspace().get(), &config);
    prompt += state.skills->prompt_suffix();
    return prompt;
}

} // namespace imza
