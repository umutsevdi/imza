#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "network/json_io.h"
#include "tools/skills.h"
#include "tools/tool.h"

TEST_CASE("builtin tools expose the current tool set")
{
    const auto tools = imza::default_tools();

    const auto* skill = find_tool(tools, "skill");
    REQUIRE(skill != nullptr);
    CHECK(skill->spec.parameters["properties"].isMember("name"));

    const auto* subagent = find_tool(tools, "subagent");
    REQUIRE(subagent != nullptr);
    CHECK(subagent->spec.parameters["properties"].isMember("tasks"));

    REQUIRE(find_tool(tools, "lua") != nullptr);
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
}

TEST_CASE("a removed native tool name is unknown to the roster")
{
    // The roster no longer varies with runtime flags: WEB/SHELL gate the
    // lua bindings, not tool membership. A native name fails dispatch.
    const auto tools = imza::default_tools();
    REQUIRE(imza::find_tool(tools, "lua") != nullptr);
    REQUIRE(imza::find_tool(tools, "skill") != nullptr);
    REQUIRE(imza::find_tool(tools, "subagent") != nullptr);

    const auto disabled = imza::dispatch_tool(
        tools, { "shell", R"({"command":"echo unavailable"})", "", "" });
    CHECK(disabled.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(disabled.text == "unknown tool: shell");
}

TEST_CASE("the skill tool handler reads instructions and records the load")
{
    const auto stamp
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("imza-skill-tool-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto path = root / "SKILL.md";
    {
        std::ofstream file(path);
        file << "documented workflow";
    }
    const imza::Skill skill { "docs", "Documentation workflow", path,
        imza::Skill::Scope::GLOBAL, std::nullopt };
    auto store = std::make_shared<imza::SkillStore>();
    imza::SkillToolDeps deps;
    deps.catalog = [skill] { return std::vector<imza::Skill> { skill }; };
    deps.store   = [store] -> imza::SkillStore& { return *store; };

    const auto tool = imza::make_skill_tool(std::move(deps));
    const auto out  = tool.run({ "skill", "", "", "" },
        imza::parse_json(R"json({"name":"docs"})json"));
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("documented workflow") != std::string::npos);
    // Loading through the tool records the skill so the gate stops asking.
    CHECK(store->is_loaded(path));

    const auto missing = tool.run({ "skill", "", "", "" },
        imza::parse_json(R"json({"name":"absent"})json"));
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(missing.text == "skill: unknown or unavailable skill");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("the skill tool records nothing without a store but still reads")
{
    const auto stamp
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("imza-skill-nostore-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto path = root / "SKILL.md";
    {
        std::ofstream file(path);
        file << "instructions";
    }
    const imza::Skill skill { "docs", "d", path, imza::Skill::Scope::GLOBAL,
        std::nullopt };
    imza::SkillToolDeps deps;
    deps.catalog    = [skill] { return std::vector<imza::Skill> { skill }; };
    const auto tool = imza::make_skill_tool(std::move(deps));
    const auto out  = tool.run({ "skill", "", "", "" },
        imza::parse_json(R"json({"name":"docs"})json"));
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
