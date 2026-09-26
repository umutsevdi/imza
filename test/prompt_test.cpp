#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "conversation/session.h"
#include "test_fs.h"
#include "turn/prompt.h"

namespace imza {

namespace {

    void write_prompt(const std::filesystem::path& directory,
        const std::string& name, const std::string& content)
    {
        std::filesystem::create_directories(directory);
        std::ofstream file(
            directory / name, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string read_prompt(std::string_view name)
    {
        std::ifstream file(
            std::filesystem::path(IMZA_PROMPT_SOURCE_DIR) / std::string(name),
            std::ios::binary);
        std::string text { std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>() };
        while (!text.empty()
            && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '
                || text.back() == '\t')) {
            text.pop_back();
        }
        return text;
    }

} // namespace

static_assert(std::is_base_of_v<ApplicationComponent, PromptStore>);

TEST_CASE("embedded prompts match the repository sources")
{
    const PromptStore prompts;
    CHECK(prompts.system() == read_prompt("system.md"));
    CHECK(prompts.subagent() == read_prompt("subagent.md"));
    CHECK(prompts.subagent_research() == read_prompt("subagent_research.md"));
    CHECK(prompts.subagent_build() == read_prompt("subagent_build.md"));
    CHECK(prompts.title() == read_prompt("title.md"));
    CHECK(prompts.compaction() == read_prompt("compaction.md"));
    CHECK(prompts.review() == read_prompt("review.md"));
    CHECK(prompts.review_plan() == read_prompt("review_plan.md"));
}

TEST_CASE("user directory overrides prompts independently")
{
    const imza::test::TempDir dir;
    const std::filesystem::path directory = dir.path;
    write_prompt(directory, "system.md", "Custom system prompt.\n");
    write_prompt(directory, "title.md", "Custom title prompt.");

    const PromptStore prompts(directory);
    CHECK(prompts.system() == "Custom system prompt.");
    CHECK(prompts.title() == "Custom title prompt.");
    CHECK(prompts.compaction() == PromptStore().compaction());
}

TEST_CASE("blank user files use embedded prompts")
{
    const imza::test::TempDir dir;
    const std::filesystem::path directory = dir.path;
    write_prompt(directory, "system.md", "   \n\t\n");

    const PromptStore prompts(directory);
    CHECK(prompts.system() == PromptStore().system());
}

TEST_CASE("prompt sources contain prose instead of runtime markup")
{
    for (const std::string_view name : { "system.md", "subagent.md",
             "subagent_research.md", "subagent_build.md", "title.md",
             "compaction.md", "review.md", "review_plan.md" }) {
        const std::string prompt = read_prompt(name);
        CHECK(prompt.find("<system-reminder") == std::string::npos);
        CHECK(prompt.find("{{") == std::string::npos);
    }
}

TEST_CASE("current mode prompts declare one authoritative state")
{
    const std::string plan = current_mode_prompt(Session::Mode::PLAN);
    CHECK(plan == "<runtime-mode name=\"plan\"/>");

    const std::string build = current_mode_prompt(Session::Mode::BUILD);
    CHECK(build == "<runtime-mode name=\"build\"/>");
}

} // namespace imza
