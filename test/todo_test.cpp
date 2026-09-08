#include <string>

#include <doctest/doctest.h>

#include "conversation/session.h"
#include "network/json_io.h"
#include "network/network.h"
#include "tools/tool.h"

TEST_CASE("parse_todo_args accepts a valid list with statuses")
{
    const auto list = imza::parse_todo_args(imza::parse_json(
        R"json({"todos":[{"content":"a","status":"in_progress"},{"content":"b","status":"completed"},{"content":"c","status":"cancelled"}]})json"));
    REQUIRE(list.has_value());
    REQUIRE(list->items.size() == 3);
    CHECK(list->items[0].content == "a");
    CHECK(list->items[0].status == imza::TodoItem::Status::IN_PROGRESS);
    CHECK(list->items[1].status == imza::TodoItem::Status::COMPLETED);
    CHECK(list->items[2].status == imza::TodoItem::Status::CANCELLED);
}

TEST_CASE("parse_todo_args defaults missing status to pending")
{
    const auto list = imza::parse_todo_args(
        imza::parse_json(R"json({"todos":[{"content":"a"}]})json"));
    REQUIRE(list.has_value());
    REQUIRE(list->items.size() == 1);
    CHECK(list->items[0].status == imza::TodoItem::Status::PENDING);
}

TEST_CASE("parse_todo_args accepts an empty list")
{
    const auto list
        = imza::parse_todo_args(imza::parse_json(R"json({"todos":[]})json"));
    REQUIRE(list.has_value());
    CHECK(list->items.empty());
}

TEST_CASE("parse_todo_args rejects malformed args")
{
    CHECK_FALSE(imza::parse_todo_args(imza::parse_json("")).has_value());
    CHECK_FALSE(imza::parse_todo_args(imza::parse_json("{}")).has_value());
    CHECK_FALSE(
        imza::parse_todo_args(imza::parse_json(R"json({"todos":"nope"})json"))
            .has_value());
    CHECK_FALSE(
        imza::parse_todo_args(imza::parse_json(R"json({"todos":[{}]})json"))
            .has_value());
    CHECK_FALSE(imza::parse_todo_args(
        imza::parse_json(R"json({"todos":[{"content":""}]})json"))
            .has_value());
    CHECK_FALSE(imza::parse_todo_args(
        imza::parse_json(
            R"json({"todos":[{"content":"a","status":"done"}]})json"))
            .has_value());
}

TEST_CASE("todo_summary renders one line per item with marks")
{
    using Status          = imza::TodoItem::Status;
    const std::string out = imza::todo_summary(imza::TodoList { {
        { "first", Status::PENDING },
        { "second", Status::IN_PROGRESS },
        { "third", Status::COMPLETED },
        { "fourth", Status::CANCELLED },
    } });
    CHECK(out == "[ ] first\n[→] second\n[x] third\n[-] fourth");
}

TEST_CASE("todo tool is registered without a direct handler")
{
    const auto tools    = imza::default_tools();
    const imza::Tool* t = imza::find_tool(tools, "todo");
    REQUIRE(t != nullptr);
    const auto out = imza::dispatch_tool(tools, { "todo", "{}" });
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
}
