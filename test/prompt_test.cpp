#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "prompt.h"
#include "tools.h"

namespace ursa {

TEST_CASE("base system prompt without environment")
{
    const std::string prompt = build_system_prompt(nullptr);
    CHECK(prompt.find("ursa") != std::string::npos);
    CHECK(prompt.find("PLAN") != std::string::npos);
    CHECK(prompt.find("BUILD") != std::string::npos);
    CHECK(prompt.find("<env>") == std::string::npos);
    CHECK(prompt.find("Available tools") == std::string::npos);
}

TEST_CASE("system prompt embeds the environment block")
{
    Environment env;
    env.os_name          = "Linux";
    env.os_version       = "6.8";
    env.distro           = "Ubuntu";
    env.default_shell    = "/bin/bash";
    env.package_managers = { "apt", "snap" };
    env.today            = "Fri Aug 28 2026";

    const std::string prompt = build_system_prompt(&env);
    CHECK(prompt.find("<env>") != std::string::npos);
    CHECK(prompt.find("Working directory") != std::string::npos);
    CHECK(prompt.find("OS: Linux 6.8") != std::string::npos);
    CHECK(prompt.find("Distro: Ubuntu") != std::string::npos);
    CHECK(prompt.find("/bin/bash") != std::string::npos);
    CHECK(prompt.find("apt, snap") != std::string::npos);
    CHECK(prompt.find("Fri Aug 28 2026") != std::string::npos);
    CHECK(prompt.find("</env>") != std::string::npos);
}

TEST_CASE("system prompt embeds workspace instructions when present")
{
    Environment env;
    env.os_name       = "Linux";
    env.default_shell = "/bin/bash";
    env.today         = "Fri Aug 28 2026";
    env.instruction   = InstructionFile { "AGENTS.md", "# Rules\nBe terse." };

    const std::string prompt = build_system_prompt(&env);
    CHECK(prompt.find("<instructions source=\"AGENTS.md\">")
        != std::string::npos);
    CHECK(prompt.find("Be terse.") != std::string::npos);
    CHECK(prompt.find("</instructions>") != std::string::npos);
}

TEST_CASE("system prompt omits the instructions block when absent")
{
    Environment env;
    env.os_name       = "Linux";
    env.default_shell = "/bin/bash";
    env.today         = "Fri Aug 28 2026";

    const std::string prompt = build_system_prompt(&env);
    CHECK(prompt.find("<instructions") == std::string::npos);
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
    const ToolRegistry tools = builtin_tools();
    const auto all = tools.specs();
    const auto read_only = tools.specs(ToolSafety::READ_ONLY);
    REQUIRE(read_only.size() < all.size());

    bool has_shell = false;
    bool has_read = false;
    bool has_ask = false;
    for (const auto& s : read_only) {
        has_shell = has_shell || s.name == "shell";
        has_read  = has_read || s.name == "read";
        has_ask   = has_ask || s.name == "ask";
    }
    CHECK_FALSE(has_shell);
    CHECK(has_read);
    CHECK(has_ask);
}

} // namespace ursa
