#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "conversation/session.h"
#include "test_fs.h"
#include "turn/prompt_store.h"

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
        // The build embeds LF-only content (the generator normalizes), while
        // a core.autocrlf checkout gives the sources CRLF; compare in the
        // embedded convention.
        std::string normalized;
        normalized.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                continue;
            }
            normalized += text[i];
        }
        text = std::move(normalized);
        while (!text.empty()
            && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '
                || text.back() == '\t')) {
            text.pop_back();
        }
        return text;
    }

} // namespace

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

TEST_CASE("current mode prompts declare one authoritative state")
{
    const std::string plan = current_mode_prompt(Session::Mode::PLAN);
    CHECK(plan == "<runtime-mode name=\"plan\"/>");

    const std::string build = current_mode_prompt(Session::Mode::BUILD);
    CHECK(build == "<runtime-mode name=\"build\"/>");
}

TEST_CASE("system prompt defers plan detail to the plan module")
{
    const std::string system = PromptStore().system();
    // Stable contract phrases, not exact prose, so wording can evolve.
    CHECK(system.find("load the `plan` module") != std::string::npos);
    CHECK(system.find("ready for build") != std::string::npos);
    CHECK(system.find("imza.build.decision") != std::string::npos);
    // The detailed plan protocol moved into the plan module's load docs.
    CHECK(system.find("imza.plan.get()") == std::string::npos);
    CHECK(system.find("imza.plan.create") == std::string::npos);
    CHECK(system.find("# Requirements") == std::string::npos);
}

TEST_CASE("plan annotations prompt derives section and line anchors")
{
    const std::string document
        = "# Requirements\nfirst plan\n# Approach\n1. first step\n"
          "2. second step\n";
    const std::vector<PlanNote> notes {
        { 5, "reorder these" },
        { 2, "sharpen the contract" },
        { 40, "gone" },
    };
    const std::string prompt = format_plan_annotations_prompt(notes, document);
    CHECK(prompt.find("Revise the current plan document") != std::string::npos);
    CHECK(prompt.find("`Approach > 2. second step`") != std::string::npos);
    CHECK(prompt.find("`Requirements > first plan`") != std::string::npos);
    CHECK(prompt.find("(stale)") != std::string::npos);
    CHECK(format_plan_annotations_prompt({ }, document).empty());
}
} // namespace imza
