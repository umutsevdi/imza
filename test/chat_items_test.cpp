#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "conversation/format.h"
#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

using imza::test::to_text;

TEST_CASE("finished turn without reasoning content hides the thought row")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("hello");
    state->session->append_item(imza::AssistantTurn {
        .reasoning        = "  \n\t",
        .reasoning_ms     = std::chrono::milliseconds { 800 },
        .reasoning_effort = "high",
    });
    auto chat
        = imza::make_chat(state, [] { return imza::test::wide_layout(); });

    std::string rendered = to_text(chat->Render(), 100, 40);
    CHECK(rendered.find("Thought") == std::string::npos);
    CHECK(rendered.find("Thinking") == std::string::npos);

    state->session->append_item(imza::AssistantTurn {
        .reasoning        = "deliberating",
        .reasoning_ms     = std::chrono::milliseconds { 800 },
        .reasoning_effort = "high",
    });
    rendered = to_text(chat->Render(), 100, 40);
    CHECK(rendered.find("Thought 0.8s") != std::string::npos);
}

TEST_CASE("chat shows planning between thought and lua execution")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("inspect the project");
    state->session->append_item(imza::AssistantTurn {
        .reasoning        = "I should inspect the files.",
        .reasoning_ms     = std::chrono::milliseconds { 1000 },
        .reasoning_effort = "high",
    });
    state->session->set_phase(imza::Session::Phase::STREAMING);

    const imza::ToolCallRequest request { "lua",
        R"json({"script":"return imza.fs.list('.')"})json", "", "call-1" };
    state->session->apply(imza::make_tool_call_start_event(request), { });
    auto chat
        = imza::make_chat(state, [] { return imza::test::wide_layout(); });

    std::string rendered          = to_text(chat->Render(), 100, 40);
    const std::size_t thought_at  = rendered.find("Thought 1.0s");
    const std::size_t planning_at = rendered.find("Planning…");
    REQUIRE(thought_at != std::string::npos);
    REQUIRE(planning_at != std::string::npos);
    CHECK(thought_at < planning_at);

    state->session->apply(imza::make_tool_call_event(request), { });
    rendered = to_text(chat->Render(), 100, 40);
    CHECK(rendered.find("Planning…") == std::string::npos);
    CHECK(rendered.find("Executing…") != std::string::npos);

    state->session->fill_tool_result(
        request, { imza::ToolCall::Result::Kind::OUTPUT, "done" });
    rendered = to_text(chat->Render(), 100, 40);
    CHECK(rendered.find("Executing…") == std::string::npos);
    CHECK(rendered.find("Executed") != std::string::npos);
}

TEST_CASE("virtual list windows the visible rows and keeps measured heights")
{
    imza::VirtualListState list(3);
    list.resize(100);
    CHECK(list.total_height() == 300);

    // Scrolled into the middle, the window is non-empty and bounded by the
    // row count.
    const imza::VirtualListWindow middle = list.window(150, 12, 0);
    CHECK(middle.begin > 0);
    CHECK(middle.end > middle.begin);
    CHECK(middle.end <= 100);
    CHECK(middle.before + middle.after > 0);

    // A measured height survives a grow and is dropped when the item goes.
    imza::VirtualListState measured(2);
    measured.resize(2);
    measured.set_height(0, 7);
    measured.resize(4);
    CHECK(measured.total_height() == 4 * 2 + 5);
    measured.resize(1);
    CHECK(measured.total_height() == 7);
}
TEST_CASE("busy chat footer shows the phase hint without a progress sweep")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("keep working");
    state->session->append_item(imza::AssistantTurn { });
    state->session->set_phase(imza::Session::Phase::STREAMING);

    auto chat
        = imza::make_chat(state, [] { return imza::test::wide_layout(); });
    const std::string rendered
        = imza::test::without_ansi(to_text(chat->Render(), 100, 40));

    bool seen_hint = false;
    for (const std::string& line : imza::split_lines(rendered)) {
        if (line.find("Ctrl+E Sidechat") == std::string::npos) {
            continue;
        }
        seen_hint = true;
        CHECK(line.find("─") == std::string::npos);
    }
    CHECK(seen_hint);
}
