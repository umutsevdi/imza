#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "common/types.h"
#include "conversation/session.h"
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
    const std::string& compaction() const { return _compaction; }
    const std::string& make_skill() const { return _make_skill; }
    const std::string& review() const { return _review; }
    const std::string& review_plan() const { return _review_plan; }
    const std::string& plan_annotations() const { return _plan_annotations; }

private:
    std::string _system;
    std::string _subagent;
    std::string _subagent_research;
    std::string _subagent_build;
    std::string _title;
    std::string _compaction;
    std::string _make_skill;
    std::string _review;
    std::string _review_plan;
    std::string _plan_annotations;
};

std::string build_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    const Config* config = nullptr);
std::string build_subagent_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    SubagentRole role, const Config* config = nullptr);
std::string title_prompt(const PromptStore& prompts, std::string_view request);
std::string current_mode_prompt(Session::Mode mode);
std::string full_system_prompt(const ApplicationState& state,
    std::optional<Session::Mode> mode = std::nullopt);
// One plan-mode revise turn for the annotator: `instructions` plus one
// bullet per note, located by the nearest heading above the pinned line and
// the current text of that line ("section > anchor" backticks). Notes whose
// line no longer exists are marked stale. Empty when there are no notes.
std::string format_plan_annotations_prompt(std::string_view instructions,
    const std::vector<PlanNote>& notes, std::string_view document);

} // namespace imza
