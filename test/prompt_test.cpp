#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "prompt.h"
#include "tools.h"

namespace ursa {

TEST_CASE("base system prompt without environment")
{
    const std::string prompt = build_system_prompt(nullptr, nullptr);
    CHECK(prompt.find("ursa") != std::string::npos);
    CHECK(prompt.find("PLAN") != std::string::npos);
    CHECK(prompt.find("BUILD") != std::string::npos);
    CHECK(prompt.find("<env>") == std::string::npos);
    CHECK(prompt.find("Available tools") == std::string::npos);
}

TEST_CASE("system prompt embeds the environment block")
{
    SystemEnvironment sys;
    sys.os_name          = "Linux";
    sys.os_version       = "6.8";
    sys.default_shell    = "/bin/bash";
    sys.package_managers = { "apt", "snap" };
    sys.today            = "Fri Aug 28 2026";

    const std::string prompt = build_system_prompt(&sys, nullptr);
    CHECK(prompt.find("<env>") != std::string::npos);
    CHECK(prompt.find("Current Directory") != std::string::npos);
    CHECK(prompt.find("Operating System: Linux 6.8") != std::string::npos);
    CHECK(prompt.find("/bin/bash") != std::string::npos);
    CHECK(prompt.find("apt, snap") != std::string::npos);
    CHECK(prompt.find("Fri Aug 28 2026") != std::string::npos);
    CHECK(prompt.find("</env>") != std::string::npos);
}

TEST_CASE("system prompt embeds workspace instructions when present")
{
    SystemEnvironment sys;
    sys.os_name       = "Linux";
    sys.default_shell = "/bin/bash";
    sys.today         = "Fri Aug 28 2026";
    WorkspaceEnvironment ws { std::filesystem::temp_directory_path() };
    ws.instruction = InstructionFile { "AGENTS.md", "# Rules\nBe terse." };

    const std::string prompt = build_system_prompt(&sys, &ws);
    CHECK(prompt.find("<instructions source=\"AGENTS.md\">")
        != std::string::npos);
    CHECK(prompt.find("Be terse.") != std::string::npos);
    CHECK(prompt.find("</instructions>") != std::string::npos);
}

TEST_CASE("system prompt omits the instructions block when absent")
{
    SystemEnvironment sys;
    sys.os_name       = "Linux";
    sys.default_shell = "/bin/bash";
    sys.today         = "Fri Aug 28 2026";
    WorkspaceEnvironment ws { std::filesystem::temp_directory_path() };

    const std::string prompt = build_system_prompt(&sys, &ws);
    CHECK(prompt.find("<instructions") == std::string::npos);
}

TEST_CASE("system prompt advertises active skills and hides denied skills")
{
    SystemEnvironment sys;
    sys.global_skills.clear();
    sys.global_skills.emplace("docs", Skill { "docs", "Write documentation",
        "/tmp/docs/SKILL.md", Skill::Scope::GLOBAL, std::nullopt });
    sys.global_skills.emplace("secret", Skill { "secret", "Hidden",
        "/tmp/secret/SKILL.md", Skill::Scope::GLOBAL, std::nullopt });
    Config config;
    config.global_skills["docs"] = SkillPolicy::ALLOW;
    config.global_skills["secret"] = SkillPolicy::DENY;
    const std::string prompt = build_system_prompt(&sys, nullptr, &config);
    CHECK(prompt.find("docs [global]: Write documentation") != std::string::npos);
    CHECK(prompt.find("secret [global]") == std::string::npos);
    CHECK(prompt.find("`skill` tool") != std::string::npos);
}

TEST_CASE("mode reminders carry unique detectable tags")
{
    const std::string_view plan = plan_mode_reminder();
    const std::string_view build = build_mode_reminder();
    CHECK(plan.find(PLAN_REMINDER_TAG) != std::string_view::npos);
    CHECK(plan.find("read-only") != std::string_view::npos);
    CHECK(build.find(BUILD_REMINDER_TAG) != std::string_view::npos);
    CHECK(plan != build);
}

TEST_CASE("specs can be filtered by safety for plan mode")
{
    const std::vector<Tool> tools = default_tools();
    const auto all = tool_specs(tools);
    const auto plan = plan_tool_specs(tools);
    REQUIRE(plan.size() < all.size());

    bool has_shell = false;
    bool has_read = false;
    bool has_ask = false;
    for (const auto& s : plan) {
        has_shell = has_shell || s.name == "shell";
        has_read  = has_read || s.name == "read";
        has_ask   = has_ask || s.name == "ask";
    }
    CHECK(has_shell);
    CHECK(has_read);
    CHECK(has_ask);
}

} // namespace ursa
