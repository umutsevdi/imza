#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "network/json_io.h"
#include "test_fs.h"
#include "tools/skills.h"
#include "tools/tool.h"

TEST_CASE("builtin tools expose the current tool set")
{
    auto state       = imza::make_lua_state();
    const auto tools = imza::default_tools({ }, { }, { }, *state);

    REQUIRE(find_tool(tools, "skill") != nullptr);
    REQUIRE(find_tool(tools, "subagent") != nullptr);
    REQUIRE(find_tool(tools, "lua") != nullptr);
    REQUIRE(find_tool(tools, "load") != nullptr);
    CHECK(find_tool(tools, "read") == nullptr);
    CHECK(find_tool(tools, "list") == nullptr);
    CHECK(find_tool(tools, "find") == nullptr);
    CHECK(find_tool(tools, "edit") == nullptr);
    CHECK(find_tool(tools, "write") == nullptr);
    CHECK(find_tool(tools, "shell") == nullptr);
    CHECK(find_tool(tools, "ask") == nullptr);
    CHECK(find_tool(tools, "todo") == nullptr);
    CHECK(find_tool(tools, "webfetch") == nullptr);
    CHECK(find_tool(tools, "websearch") == nullptr);

    // A removed native name fails dispatch: the roster no longer varies
    // with runtime flags; WEB/SHELL gate the lua bindings, not membership.
    const auto disabled = imza::dispatch_tool(
        tools, { "shell", R"({"command":"echo unavailable"})", "", "" });
    CHECK(disabled.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(disabled.text == "unknown tool: shell");
}

TEST_CASE("the skill tool handler reads instructions and records the load")
{
    const imza::test::TempDir root;
    const auto path = root.file("SKILL.md");
    imza::test::write_file(path, "documented workflow");
    const imza::Skill skill { "docs", "Documentation workflow", path,
        imza::Skill::Scope::GLOBAL, std::nullopt };
    auto store = std::make_shared<imza::SkillStore>();
    imza::SkillToolDeps deps;
    deps.catalog = [skill] { return std::vector<imza::Skill> { skill }; };
    deps.store   = [store] -> imza::SkillStore& { return *store; };

    const auto tool = imza::make_skill_tool(std::move(deps));
    const auto out
        = tool.run({ "skill", R"json({"name":"docs"})json", "", "" });
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("documented workflow") != std::string::npos);
    // Loading through the tool records the skill so the gate stops asking.
    CHECK(store->is_loaded(path));

    const auto missing
        = tool.run({ "skill", R"json({"name":"absent"})json", "", "" });
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(missing.text == "skill: unknown or unavailable skill");
}

TEST_CASE("the skill tool records nothing without a store but still reads")
{
    const imza::test::TempDir root;
    const auto path = root.file("SKILL.md");
    imza::test::write_file(path, "instructions");
    const imza::Skill skill { "docs", "d", path, imza::Skill::Scope::GLOBAL,
        std::nullopt };
    imza::SkillToolDeps deps;
    deps.catalog    = [skill] { return std::vector<imza::Skill> { skill }; };
    const auto tool = imza::make_skill_tool(std::move(deps));
    const auto out
        = tool.run({ "skill", R"json({"name":"docs"})json", "", "" });
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("instructions") != std::string::npos);
}
