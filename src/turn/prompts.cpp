#include "turn/prompts.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#include "prompt_defaults.inc"

namespace imza {

namespace {

    bool has_content(std::string_view text)
    {
        for (const char c : text) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                return true;
            }
        }
        return false;
    }

    std::string strip_trailing_space(std::string text)
    {
        while (!text.empty()
            && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '
                || text.back() == '\t')) {
            text.pop_back();
        }
        return text;
    }

    std::string load_prompt(const std::filesystem::path& overrides,
        std::string_view file_name, std::string_view fallback)
    {
        if (!overrides.empty()) {
            std::ifstream file(overrides / file_name, std::ios::binary);
            if (file) {
                std::ostringstream buffer;
                buffer << file.rdbuf();
                std::string text = buffer.str();
                if (has_content(text)) {
                    return strip_trailing_space(std::move(text));
                }
            }
        }
        return std::string(fallback);
    }

} // namespace

PromptStore::PromptStore(const std::filesystem::path& overrides)
    : _system(load_prompt(overrides, "system.md", prompts_detail::SYSTEM))
    , _subagent(load_prompt(overrides, "subagent.md", prompts_detail::SUBAGENT))
    , _subagent_research(load_prompt(
          overrides, "subagent_research.md", prompts_detail::SUBAGENT_RESEARCH))
    , _subagent_build(load_prompt(
          overrides, "subagent_build.md", prompts_detail::SUBAGENT_BUILD))
    , _title(load_prompt(overrides, "title.md", prompts_detail::TITLE))
    , _reminder_plan(load_prompt(
          overrides, "reminder_plan.md", prompts_detail::REMINDER_PLAN))
    , _reminder_build(load_prompt(
          overrides, "reminder_build.md", prompts_detail::REMINDER_BUILD))
    , _compaction(
          load_prompt(overrides, "compaction.md", prompts_detail::COMPACTION))
    , _review(load_prompt(overrides, "review.md", prompts_detail::REVIEW))
    , _review_plan(
          load_prompt(overrides, "review_plan.md", prompts_detail::REVIEW_PLAN))
{
}

const std::string& PromptStore::system() const { return _system; }

const std::string& PromptStore::subagent() const { return _subagent; }

const std::string& PromptStore::subagent_research() const
{
    return _subagent_research;
}

const std::string& PromptStore::subagent_build() const
{
    return _subagent_build;
}

const std::string& PromptStore::title() const { return _title; }

const std::string& PromptStore::reminder_plan() const { return _reminder_plan; }

const std::string& PromptStore::reminder_build() const
{
    return _reminder_build;
}

const std::string& PromptStore::compaction() const { return _compaction; }

const std::string& PromptStore::review() const { return _review; }

const std::string& PromptStore::review_plan() const { return _review_plan; }

} // namespace imza
