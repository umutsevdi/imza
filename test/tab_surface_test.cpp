#include <string>

#include <doctest/doctest.h>

#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

namespace {

imza::LayoutCtx wide_layout()
{
    return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
}

} // namespace

TEST_CASE("plan tab renders the chat")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("plan this");

    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto plan = imza::make_plan_tab(state, [] { return wide_layout(); }, chat);
    plan->Add(chat);

    const std::string rendered = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(rendered.find("plan this") != std::string::npos);
}

TEST_CASE("build tab renders the chat")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("build this");

    imza::SidechatStatus status;
    auto chat     = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto sidechat = imza::make_sidechat_component(state, [] { }, status);

    auto build = imza::make_build_tab(
        state, [] { return wide_layout(); }, chat, sidechat, status);
    build->Add(chat);

    const std::string rendered = imza::test::to_text(build->Render(), 100, 40);
    CHECK(rendered.find("build this") != std::string::npos);
}

TEST_CASE("review tab renders its pane")
{
    auto state  = imza::test::make_test_state();
    auto review = imza::make_review(
        state, [] { return wide_layout(); }, [](imza::WorkflowPhase) { });

    const std::string rendered = imza::test::to_text(review->Render(), 100, 40);
    CHECK_FALSE(rendered.empty());
}
TEST_CASE("chat hints resolve the phase line provider per render")
{
    imza::ChatHints hints;
    int calls           = 0;
    hints.phase_line_fn = [&calls] {
        ++calls;
        return calls == 1 ? std::string("no toggle here")
                          : std::string("Ctrl+S Sidechat");
    };

    auto state = imza::test::make_test_state();
    auto chat  = imza::make_chat(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 }; },
        hints);

    const std::string first = imza::test::to_text(chat->Render(), 100, 40);
    CHECK(first.find("no toggle here") != std::string::npos);
    CHECK(first.find("Ctrl+S Sidechat") == std::string::npos);

    const std::string second = imza::test::to_text(chat->Render(), 100, 40);
    CHECK(second.find("Ctrl+S Sidechat") != std::string::npos);
}
TEST_CASE("plan tab is chat-only until a plan exists")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("plan this");

    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto plan = imza::make_plan_tab(state, [] { return wide_layout(); }, chat);
    plan->Add(chat);

    // Without a plan: chat only, no doc pane, no empty-state text.
    const std::string no_plan = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(no_plan.find("plan this") != std::string::npos);
    CHECK(no_plan.find("No plan") == std::string::npos);

    REQUIRE(state->session
            ->create_plan(
                "# Goal\nx\n# Approach\nx\n# Files\nx\n# Verification\nx\n"
                "# Open Questions\nx")
            .empty());
    const std::string with_plan = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(with_plan.find("plan this") != std::string::npos);
    CHECK(with_plan.find("Initial Plan") != std::string::npos);
}
TEST_CASE("plan tab moves focus to the doc pane on click")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan("# Goal\nsecret source\n# Approach\nx\n# Files\nx\n"
                          "# Verification\nx\n# Open Questions\nx")
            .empty());

    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto plan = imza::make_plan_tab(state, [] { return wide_layout(); }, chat);
    plan->Add(chat);
    plan->TakeFocus();

    // Chat focused: the doc renders as markdown preview showing "Goal" as
    // a styled heading. Clicking the doc pane header focuses it and swaps
    // in the editor, exposing the raw source.
    const std::string chat_focused
        = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(chat_focused.find("# Goal") == std::string::npos);

    REQUIRE(imza::test::click_label(plan, "Plan"));
    const std::string doc_focused
        = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(doc_focused.find("# Goal") != std::string::npos);
}
TEST_CASE("plan tab moves focus back to the chat pane on click")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan(
                "# Goal\nx\n# Approach\nx\n# Files\nx\n# Verification\nx\n"
                "# Open Questions\nx")
            .empty());
    state->session->begin_send("build this");

    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto plan = imza::make_plan_tab(state, [] { return wide_layout(); }, chat);
    plan->Add(chat);
    plan->TakeFocus();

    REQUIRE(imza::test::click_label(plan, "Plan"));
    CHECK(imza::test::to_text(plan->Render(), 100, 40).find("# Goal")
        != std::string::npos);

    // Click the chat pane (an ASCII row, so the label offset matches the
    // click column): typing must reach the chat input again.
    REQUIRE(imza::test::click_label(plan, "Ask anything"));
    REQUIRE(plan->OnEvent(ftxui::Event::Character("q")));
    CHECK(imza::test::to_text(plan->Render(), 100, 40).find("q")
        != std::string::npos);
    CHECK(imza::test::to_text(plan->Render(), 100, 40).find("# Goal")
        == std::string::npos);
}
