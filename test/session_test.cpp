#include <variant>

#include <doctest/doctest.h>

#include "conversation/session.h"
#include "test_helpers.h"

TEST_CASE("compaction lifecycle reports the running event and signals it")
{
    imza::Session session;
    CHECK_FALSE(session.compaction_running());
    int changes = 0;
    auto subscription
        = session.subscribe_to_compaction_change([&] { ++changes; });

    const auto [id, prefix] = session.begin_compaction();
    CHECK(session.compaction_running());
    session.finish_compaction(id, "summary", prefix, true);
    CHECK_FALSE(session.compaction_running());
    CHECK(changes == 2);

    const auto [failed, prefix2] = session.begin_compaction();
    CHECK(session.compaction_running());
    session.finish_compaction(failed, "", prefix2, false);
    CHECK_FALSE(session.compaction_running());
    CHECK(changes == 4);

    subscription.disconnect();
    const auto [again, prefix3] = session.begin_compaction();
    session.finish_compaction(again, "", prefix3, false);
    CHECK(changes == 4);
}

TEST_CASE("session compaction replaces only old model history")
{
    imza::Session session;
    session.begin_send("old request");
    session.append_assistant();
    session.apply(imza::make_delta_event("old answer"), { });
    session.begin_send("current request");
    session.append_assistant();

    const auto [id, prefix] = session.begin_compaction();
    session.finish_compaction(id, "preserved summary", prefix, true);

    CHECK(session.items()->size() == 5);
    const auto history = session.build_history("system");
    REQUIRE(history.size() == 4);
    CHECK(history[1].content.find("preserved summary") != std::string::npos);
    CHECK(history[2].content == "current request");
    CHECK(history[3].type == imza::Message::Type::ASSISTANT);
}

TEST_CASE("session reports pending turns, queued messages and tools")
{
    imza::Session session;
    CHECK_FALSE(session.has_pending_work());

    session.begin_send("request");
    CHECK(session.has_pending_work());
    REQUIRE(session.finish_session(""));
    CHECK_FALSE(session.has_pending_work());

    session.enqueue_message("queued");
    CHECK(session.has_pending_work());
    REQUIRE(session.pop_queued());
    CHECK_FALSE(session.has_pending_work());

    imza::ToolCallRequest request;
    request.id   = "call-1";
    request.name = "shell";
    request.args = R"({"command":"sleep 2"})";
    imza::test::append_tool(session, request);
    CHECK(session.has_pending_work());
    session.fill_tool_result(
        request, { imza::ToolCall::Result::Kind::OUTPUT, "done" });
    CHECK_FALSE(session.has_pending_work());
}

TEST_CASE("streamed tool call starts planning then executes in place")
{
    imza::Session session;
    session.begin_send("request");
    session.append_assistant();

    imza::ToolCallRequest request;
    request.id   = "call-1";
    request.name = "lua";
    request.args = R"({"script":"return 1"})";

    session.apply(imza::make_tool_call_start_event(request), { });
    const auto planning_items = session.items();
    REQUIRE(planning_items->size() == 3);
    const auto* planning = std::get_if<imza::ToolCall>(&(*planning_items)[2]);
    REQUIRE(planning != nullptr);
    CHECK(planning->phase == imza::ToolCall::Phase::PLANNING);
    CHECK(planning->args.empty());
    CHECK(session.build_history("system").size() == 3);

    // The complete call updates that same item rather than appending a new one.
    session.apply(imza::make_tool_call_event(request), { });
    const auto executing_items = session.items();
    REQUIRE(executing_items->size() == 3);
    const auto* executing = std::get_if<imza::ToolCall>(&(*executing_items)[2]);
    REQUIRE(executing != nullptr);
    CHECK(executing->phase == imza::ToolCall::Phase::EXECUTING);
    CHECK(executing->args == request.args);
    CHECK(executing->id == planning->id);

    const auto history = session.build_history("system");
    REQUIRE(history.size() == 4);
    CHECK(history[2].tool_calls.size() == 1);

    imza::Session interrupted;
    interrupted.begin_send("request");
    interrupted.append_assistant();
    interrupted.apply(imza::make_tool_call_start_event(request), { });
    REQUIRE(interrupted.finish_session(""));
    const auto interrupted_items = interrupted.items();
    REQUIRE(interrupted_items->size() == 2);
    CHECK(std::holds_alternative<imza::AssistantTurn>((*interrupted_items)[1]));
}
