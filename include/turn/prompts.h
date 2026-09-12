#pragma once

#include <filesystem>
#include <string>

#include "common/types.h"

namespace imza {

class PromptStore final : public ApplicationComponent {
public:
    explicit PromptStore(const std::filesystem::path& overrides = { });

    const std::string& system() const;
    const std::string& subagent() const;
    const std::string& subagent_research() const;
    const std::string& subagent_build() const;
    const std::string& title() const;
    const std::string& reminder_plan() const;
    const std::string& reminder_build() const;
    const std::string& compaction() const;
    const std::string& review() const;
    const std::string& review_plan() const;

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

} // namespace imza
