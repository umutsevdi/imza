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
