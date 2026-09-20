#include <doctest/doctest.h>

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

TEST_CASE("the roster is identical in every mode and flag combination")
{
    const auto none  = imza::default_tools(imza::RuntimeFlag::NONE);
    const auto all   = imza::default_tools(imza::interactive_runtime_flags());
    const auto web   = imza::default_tools(imza::RuntimeFlag::WEB);
    const auto shell = imza::default_tools(imza::RuntimeFlag::SHELL);

    for (const auto* roster : { &none, &all, &web, &shell }) {
        REQUIRE(imza::find_tool(*roster, "lua") != nullptr);
        REQUIRE(imza::find_tool(*roster, "skill") != nullptr);
        REQUIRE(imza::find_tool(*roster, "subagent") != nullptr);
        CHECK(imza::find_tool(*roster, "shell") == nullptr);
        CHECK(imza::find_tool(*roster, "webfetch") == nullptr);
        CHECK(imza::find_tool(*roster, "ask") == nullptr);
    }

    const auto disabled = imza::dispatch_tool(
        none, { "shell", R"({"command":"echo unavailable"})", "", "" });
    CHECK(disabled.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(disabled.text == "unknown tool: shell");
}
