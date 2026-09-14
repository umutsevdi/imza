#include <string>

#include <doctest/doctest.h>

#include "conversation/session.h"
#include "network/json_io.h"

TEST_CASE("parse_todo_args accepts supported list forms")
{
    const auto statuses = imza::parse_todo_args(imza::parse_json(
        R"json({"todos":[{"content":"a","status":"in_progress"},{"content":"b","status":"completed"},{"content":"c","status":"cancelled"}]})json"));
    REQUIRE(statuses.has_value());
    REQUIRE(statuses->items.size() == 3);
    CHECK(statuses->items[0].content == "a");
    CHECK(statuses->items[0].status == imza::TodoItem::Status::IN_PROGRESS);
    CHECK(statuses->items[1].status == imza::TodoItem::Status::COMPLETED);
    CHECK(statuses->items[2].status == imza::TodoItem::Status::CANCELLED);

    const auto default_status = imza::parse_todo_args(
        imza::parse_json(R"json({"todos":[{"content":"a"}]})json"));
    REQUIRE(default_status.has_value());
    REQUIRE(default_status->items.size() == 1);
    CHECK(default_status->items[0].status == imza::TodoItem::Status::PENDING);

    const auto empty
        = imza::parse_todo_args(imza::parse_json(R"json({"todos":[]})json"));
    REQUIRE(empty.has_value());
    CHECK(empty->items.empty());
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
