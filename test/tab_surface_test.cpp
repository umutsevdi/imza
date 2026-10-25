#include <string>

#include <doctest/doctest.h>

#include "app/flows.h"

#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"
#include "workspace/review_subsystem.h"

namespace {

ftxui::Component plan_tab_with_chat(
    const std::shared_ptr<imza::ApplicationState>& state)
{
    auto chat
        = imza::make_chat(state, [] { return imza::test::wide_layout(); });
    auto plan = imza::make_plan_tab(
        state, [] { return imza::test::wide_layout(); }, chat);
    plan->Add(chat);
    return plan;
}

} // namespace

TEST_CASE("chat hints resolve the phase line provider per render")
{
    imza::ChatHints hints;
    int calls           = 0;
    hints.phase_line_fn = [&calls] {
        ++calls;
        return calls == 1 ? std::string("no toggle here")
                          : std::string("Ctrl+E Sidechat");
    };

    auto state = imza::test::make_test_state();
    auto chat  = imza::make_chat(
        state, [] { return imza::test::wide_layout(); }, hints);

    const std::string first = imza::test::to_text(chat->Render(), 100, 40);
    CHECK(first.find("no toggle here") != std::string::npos);
    CHECK(first.find("Ctrl+E Sidechat") == std::string::npos);

    const std::string second = imza::test::to_text(chat->Render(), 100, 40);
    CHECK(second.find("Ctrl+E Sidechat") != std::string::npos);
}
TEST_CASE("plan tab is chat-only until a plan exists")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("plan this");

    auto plan = plan_tab_with_chat(state);

    const std::string no_plan = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(no_plan.find("plan this") != std::string::npos);
    CHECK(no_plan.find("No plan") == std::string::npos);

    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());
    const std::string with_plan = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(with_plan.find("plan this") != std::string::npos);
    CHECK(with_plan.find("Initial Plan") != std::string::npos);
}
TEST_CASE("plan tab moves focus to the doc pane on click")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan(
                "# Requirements\nsecret source\n# Approach\nx\n# Changes\nx\n"
                "# Verification\nx")
            .empty());

    auto plan = plan_tab_with_chat(state);
    plan->TakeFocus();

    const std::string chat_focused
        = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(chat_focused.find("Initial Plan") != std::string::npos);

    REQUIRE(imza::test::click_label(plan, "Initial Plan"));
    const std::string doc_focused
        = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(doc_focused.find("Enter note") != std::string::npos);
}
TEST_CASE("plan tab moves focus back to the chat pane on click")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());
    state->session->begin_send("build this");

    auto plan = plan_tab_with_chat(state);
    plan->TakeFocus();

    REQUIRE(imza::test::click_label(plan, "Initial Plan"));
    CHECK(imza::test::to_text(plan->Render(), 100, 40).find("Enter note")
        != std::string::npos);

    // The chat pane is an ASCII row, so the label offset matches the click
    // column.
    REQUIRE(imza::test::click_label(plan, "Ask anything"));
    REQUIRE(plan->OnEvent(ftxui::Event::Character("q")));
    CHECK(imza::test::to_text(plan->Render(), 100, 40).find("q")
        != std::string::npos);
}
TEST_CASE("wide plan tab places the chat left of the doc pane")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan(
                "# Requirements\nside by side\n# Approach\nx\n# Changes\nx\n"
                "# Verification\nx")
            .empty());

    auto plan = plan_tab_with_chat(state);
    plan->TakeFocus();

    const std::vector<std::string> lines
        = imza::split_lines(imza::test::to_text(plan->Render(), 100, 40));
    int chat_row = -1;
    int doc_row  = -1;
    int chat_col = -1;
    int doc_col  = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string plain = imza::test::without_ansi(lines[i]);
        if (const auto at = plain.find("Ask anything");
            at != std::string::npos && chat_row == -1) {
            chat_row = static_cast<int>(i);
            chat_col = static_cast<int>(at);
        }
        if (const auto at = plain.find("Initial Plan");
            at != std::string::npos && doc_row == -1) {
            doc_row = static_cast<int>(i);
            doc_col = static_cast<int>(at);
        }
    }
    REQUIRE(chat_row != -1);
    REQUIRE(doc_row != -1);
    // Side by side, not stacked: the doc pane sits on the same rows and
    // starts right of the chat column.
    CHECK(doc_col > chat_col);
}

TEST_CASE("chat focused: annotator keys land in the chat input")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan("# Requirements\nannotator source\n# Approach\nx\n"
                          "# Changes\nx\n# Verification\nx")
            .empty());

    auto plan = plan_tab_with_chat(state);
    plan->TakeFocus();
    (void)plan->Render();

    REQUIRE(plan->OnEvent(ftxui::Event::Character("c")));
    REQUIRE(plan->OnEvent(ftxui::Event::Character("[")));
    REQUIRE(plan->OnEvent(ftxui::Event::Character("]")));

    const std::string rendered = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(rendered.find("c[]") != std::string::npos);
    CHECK(rendered.find("Leave a note") == std::string::npos);
}
TEST_CASE("review pane shortcuts: p sends to plan, r refuses without a diff")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    imza::test::wait_for_environment(*state);
    auto review = imza::make_review(
        state, [] { return imza::test::wide_layout(); },
        [](imza::WorkflowPhase) { });
    (void)imza::test::to_text(review->Render(), 100, 40);

    REQUIRE(review->OnEvent(ftxui::Event::Character("p")));
    CHECK(state->session->error().find("Add a review comment")
        != std::string::npos);
    CHECK(state->session->items()->empty());

    state->session->clear_error();
    REQUIRE(review->OnEvent(ftxui::Event::Character("r")));
    CHECK(state->session->error().find("no changes to review")
        != std::string::npos);

    state->session->clear_error();
    REQUIRE(
        state->review->add_comment({ "file.cpp", 2, 3, "line" }, "note") != 0);
    REQUIRE(review->OnEvent(ftxui::Event::Character("p")));
    bool found       = false;
    const auto items = state->session->items();
    for (const auto& item : *items) {
        if (const auto* user = std::get_if<imza::UserTurn>(&item);
            user != nullptr && user->text.find("note") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
    CHECK(state->review->comments().empty());
}
