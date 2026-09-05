#include <string>

#include <doctest/doctest.h>

#include "agent/tools.h"
#include "network/json_io.h"
#include "network/network.h"
#include "subsystems/session.h"

TEST_CASE("parse_todo_args accepts a valid list with statuses")
{
    const auto list = ursa::parse_todo_args(ursa::parse_json(
        R"json({"todos":[{"content":"a","status":"in_progress"},{"content":"b","status":"completed"},{"content":"c","status":"cancelled"}]})json"));
    REQUIRE(list.has_value());
    REQUIRE(list->items.size() == 3);
    CHECK(list->items[0].content == "a");
    CHECK(list->items[0].status == ursa::TodoItem::Status::IN_PROGRESS);
    CHECK(list->items[1].status == ursa::TodoItem::Status::COMPLETED);
    CHECK(list->items[2].status == ursa::TodoItem::Status::CANCELLED);
}

TEST_CASE("parse_todo_args defaults missing status to pending")
{
    const auto list = ursa::parse_todo_args(
        ursa::parse_json(R"json({"todos":[{"content":"a"}]})json"));
    REQUIRE(list.has_value());
    REQUIRE(list->items.size() == 1);
    CHECK(list->items[0].status == ursa::TodoItem::Status::PENDING);
}

TEST_CASE("parse_todo_args accepts an empty list")
{
    const auto list
        = ursa::parse_todo_args(ursa::parse_json(R"json({"todos":[]})json"));
    REQUIRE(list.has_value());
    CHECK(list->items.empty());
}

TEST_CASE("parse_todo_args rejects malformed args")
{
    CHECK_FALSE(ursa::parse_todo_args(ursa::parse_json("")).has_value());
    CHECK_FALSE(ursa::parse_todo_args(ursa::parse_json("{}")).has_value());
    CHECK_FALSE(
        ursa::parse_todo_args(ursa::parse_json(R"json({"todos":"nope"})json"))
            .has_value());
    CHECK_FALSE(
        ursa::parse_todo_args(ursa::parse_json(R"json({"todos":[{}]})json"))
            .has_value());
    CHECK_FALSE(ursa::parse_todo_args(
        ursa::parse_json(R"json({"todos":[{"content":""}]})json"))
            .has_value());
    CHECK_FALSE(ursa::parse_todo_args(
        ursa::parse_json(
            R"json({"todos":[{"content":"a","status":"done"}]})json"))
            .has_value());
}

TEST_CASE("todo_summary renders one line per item with marks")
{
    using Status          = ursa::TodoItem::Status;
    const std::string out = ursa::todo_summary(ursa::TodoList { {
        { "first", Status::PENDING },
        { "second", Status::IN_PROGRESS },
        { "third", Status::COMPLETED },
        { "fourth", Status::CANCELLED },
    } });
    CHECK(out == "[ ] first\n[→] second\n[x] third\n[-] fourth");
}

TEST_CASE("todo tool is registered read-only without a direct handler")
{
    const auto tools    = ursa::default_tools();
    const ursa::Tool* t = ursa::find_tool(tools, "todo");
    REQUIRE(t != nullptr);
    CHECK(t->safety == ursa::ToolSafety::READ_ONLY);
    const auto out = ursa::dispatch_tool(tools, { "todo", "{}" });
    CHECK(out.kind == ursa::ToolOutput::Kind::ERROR);
}
