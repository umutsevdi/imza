#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "conversation/session.h"
#include "tools/tool.h"
#include "turn/prompt.h"

namespace imza {

TEST_CASE("base system prompt without environment")
{
    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, nullptr, nullptr);
    CHECK(prompt.find("imza") != std::string::npos);
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

    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, &sys, nullptr);
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
    WorkspaceEnvironment ws;
    ws.working_directory = std::filesystem::temp_directory_path();
    ws.instruction = InstructionFile { "AGENTS.md", "# Rules\nBe terse." };

    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, &sys, &ws);
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
    WorkspaceEnvironment ws;
    ws.working_directory = std::filesystem::temp_directory_path();

    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, &sys, &ws);
    CHECK(prompt.find("<instructions") == std::string::npos);
}

TEST_CASE("system prompt advertises active skills and hides denied skills")
{
    SystemEnvironment sys;
    sys.global_skills.clear();
    sys.global_skills.emplace("docs",
        Skill { "docs", "Write documentation", "/tmp/docs/SKILL.md",
            Skill::Scope::GLOBAL, std::nullopt });
    sys.global_skills.emplace("secret",
        Skill { "secret", "Hidden", "/tmp/secret/SKILL.md",
            Skill::Scope::GLOBAL, std::nullopt });
    Config config;
    config.global_skills["docs"]   = SkillPolicy::ALLOW;
    config.global_skills["secret"] = SkillPolicy::DENY;
    const PromptStore prompts;
    const std::string prompt
        = build_system_prompt(prompts, &sys, nullptr, &config);
    CHECK(
        prompt.find("docs [global]: Write documentation") != std::string::npos);
    CHECK(prompt.find("secret [global]") == std::string::npos);
    CHECK(prompt.find("`skill` tool") != std::string::npos);
}

TEST_CASE("research subagent prompt is dedicated and read-only")
{
    const PromptStore prompts;
    const std::string prompt = build_subagent_system_prompt(
        prompts, nullptr, nullptr, SubagentRole::RESEARCH);
    CHECK(prompt.find("Imza subagent") != std::string::npos);
    CHECK(prompt.find("fresh context") != std::string::npos);
    CHECK(prompt.find("Work read-only") != std::string::npos);
    CHECK(prompt.find("implementation plan") != std::string::npos);
    CHECK(prompt.find("# Todo list") == std::string::npos);
    CHECK(prompt.find("interactive CLI coding agent") == std::string::npos);
}

TEST_CASE("build subagent prompt permits focused changes")
{
    const PromptStore prompts;
    const std::string prompt = build_subagent_system_prompt(
        prompts, nullptr, nullptr, SubagentRole::BUILDER);
    CHECK(prompt.find("may modify files") != std::string::npos);
    CHECK(prompt.find("keep changes focused") != std::string::npos);
    CHECK(prompt.find("Work read-only") == std::string::npos);
}

TEST_CASE("basic subagent has no system prompt")
{
    const PromptStore prompts;
    CHECK(build_subagent_system_prompt(
        prompts, nullptr, nullptr, SubagentRole::BASIC)
            .empty());
}

TEST_CASE("subagent prompt retains workspace context")
{
    SystemEnvironment sys;
    sys.os_name       = "Linux";
    sys.default_shell = "/bin/bash";
    sys.today         = "Tue Sep 1 2026";
    sys.global_skills.emplace("docs",
        Skill { "docs", "Write documentation", "/tmp/docs/SKILL.md",
            Skill::Scope::GLOBAL, std::nullopt });
    WorkspaceEnvironment ws;
    ws.working_directory = std::filesystem::temp_directory_path();
    ws.instruction = InstructionFile { "AGENTS.md", "Use project rules." };

    const PromptStore prompts;
    const std::string prompt = build_subagent_system_prompt(
        prompts, &sys, &ws, SubagentRole::BUILDER);
    CHECK(prompt.find("Operating System: Linux") != std::string::npos);
    CHECK(prompt.find("<instructions source=\"AGENTS.md\">")
        != std::string::npos);
    CHECK(prompt.find("Use project rules.") != std::string::npos);
    CHECK(
        prompt.find("docs [global]: Write documentation") != std::string::npos);
    CHECK(prompt.find("$skill-name") == std::string::npos);
}

TEST_CASE("default tool set contains build mutation tools")
{
    const std::vector<Tool> tools = default_tools();
    const auto all                = tool_specs(tools);
    CHECK(all.size() == tools.size());
    CHECK(std::any_of(all.begin(), all.end(),
        [](const ToolSpec& spec) { return spec.name == "edit"; }));
    CHECK(std::any_of(all.begin(), all.end(),
        [](const ToolSpec& spec) { return spec.name == "write"; }));
}

} // namespace imza
