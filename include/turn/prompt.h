#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "common/types.h"
#include "platform/config.h"
#include "workspace/environment.h"

namespace imza {

struct ApplicationState;

class PromptStore final : public ApplicationComponent {
public:
    explicit PromptStore(const std::filesystem::path& overrides = { });

    const std::string& system() const { return _system; }
    const std::string& subagent() const { return _subagent; }
    const std::string& subagent_research() const { return _subagent_research; }
    const std::string& subagent_build() const { return _subagent_build; }
    const std::string& title() const { return _title; }
    const std::string& reminder_plan() const { return _reminder_plan; }
    const std::string& reminder_build() const { return _reminder_build; }
    const std::string& compaction() const { return _compaction; }
    const std::string& review() const { return _review; }
    const std::string& review_plan() const { return _review_plan; }

private:
    std::string _system;
    std::string _subagent;
    std::string _subagent_research;
    std::string _subagent_build;
    std::string _title;
    std::string _reminder_plan;
    std::string _reminder_build;
    std::string _compaction;
    std::string _review;
    std::string _review_plan;
};

std::string build_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    const Config* config = nullptr);
std::string build_subagent_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    SubagentRole role, const Config* config = nullptr);
std::string title_prompt(const PromptStore& prompts, std::string_view request);
std::string plan_mode_reminder(const PromptStore& prompts);
std::string build_mode_reminder(const PromptStore& prompts);
std::string full_system_prompt(const ApplicationState& state);

} // namespace imza
