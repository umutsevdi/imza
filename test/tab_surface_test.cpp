#include <string>

#include <doctest/doctest.h>

#include "app/flows.h"

#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

namespace {

imza::LayoutCtx wide_layout()
{
    return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
}

TEST_CASE("sidechat and main chat fill the wide layout when open")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("hello there this is the main chat");
    imza::SidechatStatus status;
    auto chat     = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 };
    });
    auto sidechat = imza::make_sidechat_component(state, [] { }, status);
    imza::open_sidechat(*state);
    (void)chat->Render();
    (void)sidechat->Render();

    using namespace ftxui;
    Element content = vbox({ text("TITLE ROW") }) | xflex | yflex;
    content
        = hbox({ std::move(content), separatorEmpty(), sidechat->Render() });
    Element main_panel = vbox({ hbox({ text(" "), text("title") | xflex }),
                             std::move(content) })
        | xflex | yflex;
    Element root = vbox({ std::move(main_panel) | flex }) | flex;
    auto screen  = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen, root);
    const std::vector<std::string> raw_lines
        = imza::split_lines(screen.ToString());
    // The sidechat column must stretch to the bottom: its input panel
    // renders in the lower rows of the 50-wide right column.
    bool sidechat_reaches_bottom = false;
    for (int y = 34; y < 40 && y < static_cast<int>(raw_lines.size()); ++y) {
        const std::string clean = imza::test::without_ansi(raw_lines[y]);
        if (clean.size() > 70
            && clean.find_first_not_of(' ', 70) != std::string::npos) {
            sidechat_reaches_bottom = true;
        }
        MESSAGE("[", clean, "]");
    }
    CHECK(sidechat_reaches_bottom);
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

    // Chat focused: the annotator pane renders dim and ignores keys.
    const std::string chat_focused
        = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(chat_focused.find("Initial Plan") != std::string::npos);

    // Clicking the doc pane focuses it and the annotator keys light up.
    REQUIRE(imza::test::click_label(plan, "Initial Plan"));
    const std::string doc_focused
        = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(doc_focused.find("c note") != std::string::npos);
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

    REQUIRE(imza::test::click_label(plan, "Initial Plan"));
    CHECK(imza::test::to_text(plan->Render(), 100, 40).find("c note")
        != std::string::npos);

    // Click the chat pane (an ASCII row, so the label offset matches the
    // click column): typing must reach the chat input again.
    REQUIRE(imza::test::click_label(plan, "Ask anything"));
    REQUIRE(plan->OnEvent(ftxui::Event::Character("q")));
    CHECK(imza::test::to_text(plan->Render(), 100, 40).find("q")
        != std::string::npos);
}
TEST_CASE("wide plan tab places the chat left of the doc pane")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan("# Goal\nside by side\n# Approach\nx\n# Files\nx\n"
                          "# Verification\nx\n# Open Questions\nx")
            .empty());

    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto plan = imza::make_plan_tab(state, [] { return wide_layout(); }, chat);
    plan->Add(chat);
    plan->TakeFocus();

    const std::string rendered = imza::test::to_text(plan->Render(), 100, 40);
    const std::vector<std::string> lines = imza::split_lines(rendered);
    int chat_column                      = -1;
    int doc_column                       = -1;
    int doc_row                          = -1;
    for (int y = 0; y < static_cast<int>(lines.size()); ++y) {
        const std::string plain = imza::test::without_ansi(lines[y]);
        if (const auto at = plain.find("Initial Plan");
            at != std::string::npos) {
            doc_column = static_cast<int>(at);
            doc_row    = y;
        }
        if (const auto at = plain.find("Ask anything");
            at != std::string::npos) {
            chat_column = static_cast<int>(at);
        }
    }
    REQUIRE(doc_column != -1);
    REQUIRE(chat_column != -1);
    // Same row region: the input row and the doc header sit in the same
    // half of the pane; the doc starts right of the chat column.
    CHECK(doc_column > chat_column);
    (void)doc_row;
}

TEST_CASE("chat focused: annotator keys land in the chat input")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan(
                "# Goal\nannotator source\n# Approach\nx\n# Files\nx\n"
                "# Verification\nx\n# Open Questions\nx")
            .empty());

    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto plan = imza::make_plan_tab(state, [] { return wide_layout(); }, chat);
    plan->Add(chat);
    plan->TakeFocus();
    (void)plan->Render();

    // The chat owns focus; printable annotator keys must reach it.
    REQUIRE(plan->OnEvent(ftxui::Event::Character("c")));
    REQUIRE(plan->OnEvent(ftxui::Event::Character("[")));
    REQUIRE(plan->OnEvent(ftxui::Event::Character("]")));

    const std::string rendered = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(rendered.find("c[]") != std::string::npos);
    CHECK(rendered.find("Leave a note") == std::string::npos);
}
