#include <chrono>
#include <functional>
#include <string>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "conversation/format.h"
#include "test_helpers.h"
#include "ui/ui.h"

using imza::test::to_text;

TEST_CASE("render_item renders user attachment labels")
{
    imza::ConversationItem it
        = imza::UserTurn { "review", { { "src/main.cpp", "int main() {}" } } };
    const std::string out
        = to_text(imza::render_item(it, { imza::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("@src/main.cpp") != std::string::npos);
}

TEST_CASE("render_item renders completed compaction")
{
    imza::ConversationItem item
        = imza::CompactionEvent { 1, imza::CompactionEvent::Status::COMPLETED };
    const std::string out
        = to_text(imza::render_item(item, { imza::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("✓ Session compacted") != std::string::npos);
}

TEST_CASE("chat shows planning between thought and lua execution")
{
    auto state = imza::make_application_state(
        [](std::function<void()> fn) { fn(); }, imza::Config { });
    state->session->begin_send("inspect the project");
    state->session->append_item(imza::AssistantTurn {
        .reasoning        = "I should inspect the files.",
        .reasoning_ms     = std::chrono::milliseconds { 1000 },
        .reasoning_effort = "high",
    });
    state->session->set_phase(imza::Session::Phase::STREAMING);

    const imza::ToolCallRequest request { "lua",
        R"json({"script":"return tool.list('.')"})json", "", "call-1" };
    state->session->apply(imza::make_tool_call_start_event(request), { });
    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });

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

TEST_CASE("virtual list selects only the visible rows")
{
    imza::VirtualListState list(3);
    list.resize(100);

    const imza::VirtualListWindow window = list.window(150, 12, 0);

    CHECK(window.begin == 50);
    CHECK(window.end == 54);
    CHECK(window.before == 150);
    CHECK(window.after == 138);
    CHECK(list.total_height() == 300);
}

TEST_CASE("virtual list preserves measured heights and applies overscan")
{
    imza::VirtualListState list(3);
    list.resize(5);
    CHECK(list.set_height(0, 8) == 5);
    CHECK(list.set_height(1, 5) == 2);
    CHECK(list.set_height(1, 5) == 0);

    const imza::VirtualListWindow window = list.window(9, 3, 4);

    CHECK(window.begin == 0);
    CHECK(window.end == 3);
    CHECK(window.before == 0);
    CHECK(window.after == 6);
    CHECK(list.total_height() == 22);
}

TEST_CASE("virtual list retains measurements while growing")
{
    imza::VirtualListState list(2);
    list.resize(2);
    list.set_height(0, 7);
    list.resize(4);

    CHECK(list.total_height() == 13);
    CHECK(list.window(7, 2, 0).begin == 1);

    list.resize(1);
    CHECK(list.total_height() == 7);
}
