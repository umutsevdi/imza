#include <chrono>
#include <string>
#include <thread>

#include <doctest/doctest.h>

#include <ftxui/component/event.hpp>

#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

namespace {

imza::LayoutCtx wide_layout()
{
    return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
}

const std::string skeleton
    = "# Goal\nfirst plan\n# Approach\n1. first step\n2. second step\n"
      "# Files\nx\n# Verification\ncheck it\n# Open Questions\nx";

} // namespace

TEST_CASE("plan doc renders the initial plan as block rows when unfocused")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);

    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("Plan") != std::string::npos);
    CHECK(rendered.find("Initial Plan") != std::string::npos);
    CHECK(rendered.find("first plan") != std::string::npos);
    CHECK(rendered.find("second step") != std::string::npos);
}

TEST_CASE("plan doc updates live and labels agent revisions")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    REQUIRE(
        state->session->create_plan(skeleton + "\nextra note from the agent")
            .empty());
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("Revision 1") != std::string::npos);
    CHECK(rendered.find("extra note from the agent") != std::string::npos);
}

TEST_CASE("note editor opens on c and saves on Enter")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    const std::string editor = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(editor.find("Leave a note") != std::string::npos);

    for (const char c : std::string("make it faster")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));

    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("make it faster") != std::string::npos);
    CHECK(rendered.find("1 notes") != std::string::npos);
}

TEST_CASE("note card renders under the annotated block")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // Walk to the second list item: heading Goal, first plan, heading
    // Approach, first item, second item.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(doc->OnEvent(ftxui::Event::ArrowDown));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("split this step")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));

    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    const auto note_at         = rendered.find("split this step");
    const auto item_at         = rendered.find("second step");
    REQUIRE(note_at != std::string::npos);
    REQUIRE(item_at != std::string::npos);
    CHECK(note_at > item_at);
}

TEST_CASE("revise submits one turn with section and line locators")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // Walk to the second list item: heading Goal, first plan, heading
    // Approach, first item, second item.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(doc->OnEvent(ftxui::Event::ArrowDown));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("reorder these")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    REQUIRE(doc->OnEvent(ftxui::Event::Character("s")));

    // The revise turn starts immediately (a provider is configured); the
    // user turn carries the derived locators and the note body.
    REQUIRE(state->session->items().size() >= 1);
    const auto* user
        = std::get_if<imza::UserTurn>(&state->session->items().front());
    REQUIRE(user != nullptr);
    CHECK(user->text.find("Approach > 2. second step") != std::string::npos);
    CHECK(user->text.find("reorder these") != std::string::npos);
    // Notes clear after the revise turn is sent.
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("reorder these") == std::string::npos);
}

TEST_CASE("unfocused pane ignores annotator keys and opens no editor")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    CHECK_FALSE(doc->OnEvent(ftxui::Event::Character("c")));
    CHECK_FALSE(doc->OnEvent(ftxui::Event::Character("s")));
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("Leave a note") == std::string::npos);
    CHECK(rendered.find("first plan") != std::string::npos);
}

TEST_CASE("e and d edit and delete the selected note")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("first draft")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));

    // The cursor parks on the saved note card after the next render;
    // edit it.
    (void)doc->Render();
    REQUIRE(doc->OnEvent(ftxui::Event::Character("e")));
    for (const char c : std::string(" plus")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    CHECK(imza::test::to_text(doc->Render(), 100, 40).find("first draft plus")
        != std::string::npos);

    // Delete it from the note row.
    REQUIRE(doc->OnEvent(ftxui::Event::Character("d")));
    CHECK(imza::test::to_text(doc->Render(), 100, 40).find("first draft")
        == std::string::npos);
}

TEST_CASE("section jumps with brackets and a heading note anchors the section")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // ] jumps to the next section heading (Approach); a note on the
    // heading row anchors the whole section without a block excerpt.
    REQUIRE(doc->OnEvent(ftxui::Event::Character("]")));
    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("too detailed")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    REQUIRE(doc->OnEvent(ftxui::Event::Character("s")));

    REQUIRE(state->session->items().size() >= 1);
    const auto* user
        = std::get_if<imza::UserTurn>(&state->session->items().front());
    REQUIRE(user != nullptr);
    // The heading note pins the heading line; the locator cites the
    // section with the heading line itself as the edit anchor.
    CHECK(user->text.find("`Approach > # Approach`") != std::string::npos);
    CHECK(user->text.find("too detailed") != std::string::npos);
}

TEST_CASE("plan doc without a plan shows the empty hint")
{
    auto state   = imza::test::make_test_state();
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("No plan") != std::string::npos);
}

TEST_CASE("plan created after pane construction renders immediately")
{
    auto state   = imza::test::make_test_state();
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);

    const std::string empty = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(empty.find("No plan") != std::string::npos);

    REQUIRE(state->session
            ->create_plan("# Goal\nlate draft\n# Approach\nx\n# Files\nx\n# "
                          "Verification\nx\n"
                          "# Open Questions\nx")
            .empty());
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("late draft") != std::string::npos);
    CHECK(rendered.find("Initial Plan") != std::string::npos);
}

TEST_CASE("long document lines wrap inside the pane instead of overflowing")
{
    auto state                  = imza::test::make_test_state();
    const std::string long_line = "# Goal\n" + std::string(300, 'x')
        + "\n# Approach\nx\n# Files\nx\n# Verification\nx\n"
          "# Open Questions\nx";
    REQUIRE(state->session->create_plan(long_line).empty());
    bool focused = false;

    auto doc = imza::make_plan_doc(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 60, 40 }; },
        &focused);

    const std::string rendered = imza::test::to_text(doc->Render(), 60, 40);
    // Every rendered row stays inside the pane width; measure the visible
    // cells (ANSI styling is stripped, trailing padding trimmed).
    for (const std::string& line : imza::split_lines(rendered)) {
        const std::string visible = imza::test::without_ansi(line);
        // Cell width: UTF-8 codepoints, trailing padding excluded.
        std::size_t end = visible.size();
        while (
            end > 0 && (visible[end - 1] == ' ' || visible[end - 1] == '\r')) {
            --end;
        }
        int codepoints = 0;
        for (std::size_t i = 0; i < end;) {
            const unsigned char c = static_cast<unsigned char>(visible[i]);
            i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            ++codepoints;
        }
        CHECK(codepoints <= 60);
    }
}
TEST_CASE("click on a block row opens the note editor inline")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // Click the "first plan" block row.
    REQUIRE(imza::test::click_label(doc, "first plan"));

    // The editor card renders in-flow between the clicked block and the
    // next block row, not at the bottom bar.
    const std::vector<std::string> lines
        = imza::split_lines(imza::test::to_text(doc->Render(), 100, 40));
    int editor_at = -1;
    int next_at   = -1;
    int plan_at   = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string clean = imza::test::without_ansi(lines[i]);
        if (editor_at == -1
            && clean.find("Leave a note") != std::string::npos) {
            editor_at = static_cast<int>(i);
        }
        if (plan_at == -1 && clean.find("first plan") != std::string::npos) {
            plan_at = static_cast<int>(i);
        }
        if (clean.find("Approach") != std::string::npos) {
            next_at = static_cast<int>(i);
        }
    }
    REQUIRE(editor_at != -1);
    CHECK(editor_at > plan_at);
    CHECK(editor_at < next_at);
    CHECK(imza::test::to_text(doc->Render(), 100, 40).find("Revise Plan")
        != std::string::npos);

    // Escape closes the editor; typing and Enter still save.
    REQUIRE(doc->OnEvent(ftxui::Event::Escape));
    const std::string closed = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(closed.find("Leave a note") == std::string::npos);
}

TEST_CASE("moving with keys highlights the selected row")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // The raw render carries background escapes; find which visible line
    // holds the panel-focus background and follow it as the cursor moves.
    // The editor card and note cards share the focus background, so filter
    // to card-free rows by text: the block text itself.
    const auto highlight_at = [](const std::string& rendered,
                                  const std::string& needle) {
        const std::vector<std::string> lines = imza::split_lines(rendered);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("\x1b[48;2;") != std::string::npos
                && imza::test::without_ansi(lines[i]).find(needle)
                    != std::string::npos) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    // Row 0 is selected: the "Goal" heading row is highlighted.
    CHECK(highlight_at(imza::test::to_text(doc->Render(), 100, 40), "Goal")
        != -1);

    // Move down twice: row 2 is the "Approach" heading; it is highlighted
    // and "Goal" is not.
    REQUIRE(doc->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(doc->OnEvent(ftxui::Event::ArrowDown));
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(highlight_at(rendered, "Approach") != -1);
    CHECK(highlight_at(rendered, "Goal") == -1);
}

TEST_CASE("click on a note card selects it for edit and delete")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();
    REQUIRE(imza::test::click_label(doc, "first plan"));
    for (const char c : std::string("annotate this")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    (void)doc->Render();

    // Clicking elsewhere selects a block row (and opens the editor);
    // closing it leaves the cursor off the card, so e has nothing to edit.
    REQUIRE(imza::test::click_label(doc, "Open Questions"));
    REQUIRE(doc->OnEvent(ftxui::Event::Escape));
    REQUIRE_FALSE(doc->OnEvent(ftxui::Event::Character("e")));

    // Clicking the card selects it; e focuses the editor with the body.
    REQUIRE(imza::test::click_label(doc, "annotate this"));
    REQUIRE(doc->OnEvent(ftxui::Event::Character("e")));
    for (const char c : std::string(" more")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("annotate this more") != std::string::npos);

    REQUIRE(imza::test::click_label(doc, "annotate this"));
    REQUIRE(doc->OnEvent(ftxui::Event::Character("d")));
    CHECK(imza::test::to_text(doc->Render(), 100, 40).find("annotate this")
        == std::string::npos);
}

TEST_CASE("space opens a note instead of triggering revise")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    REQUIRE(doc->OnEvent(ftxui::Event::Character(" ")));
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("Leave a note") != std::string::npos);
    // No revise turn was submitted.
    CHECK(state->session->items().empty());
}

TEST_CASE("revise button click submits the notes")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("tighten scope")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    (void)doc->Render();

    REQUIRE(imza::test::click_label(doc, "Revise Plan"));
    REQUIRE(state->session->items().size() >= 1);
    const auto* user
        = std::get_if<imza::UserTurn>(&state->session->items().front());
    REQUIRE(user != nullptr);
    CHECK(user->text.find("tighten scope") != std::string::npos);
}

TEST_CASE("revise works repeatedly without a plan change in between")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);

    // First revise via the keyboard.
    (void)doc->Render();
    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("first pass")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    (void)doc->Render();
    REQUIRE(doc->OnEvent(ftxui::Event::Character("s")));
    REQUIRE(state->session->items().size() >= 1);

    // The agent answers without touching the plan; a second note and the
    // button must still submit (no revise latch). The pane re-renders
    // between keystrokes in the app, so mirror that here.
    (void)doc->Render();
    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("second pass")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    (void)doc->Render();
    REQUIRE(imza::test::click_label(doc, "Revise Plan"));
    bool found = false;
    for (const auto& item : state->session->items()) {
        const auto* user = std::get_if<imza::UserTurn>(&item);
        if (user != nullptr
            && user->text.find("second pass") != std::string::npos) {
            found = true;
        }
    }
    // A busy session queues the revise instead of dropping it.
    for (const auto& queued : state->session->queued()) {
        if (queued.text.find("second pass") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("revise button highlights on click like review buttons")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;
    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    const auto button_row = [](const std::string& rendered) {
        for (const std::string& line : imza::split_lines(rendered)) {
            if (imza::test::without_ansi(line).find("Revise Plan")
                != std::string::npos) {
                return line;
            }
        }
        return std::string { };
    };
    const std::string rest
        = button_row(imza::test::to_text(doc->Render(), 100, 40));
    // Rest: calm background (not the focus panel), not bold.
    CHECK(rest.find("\x1b[48;2;72;79;88m") != std::string::npos);
    CHECK(rest.find("\x1b[1m") == std::string::npos);

    // Clicking the button submits and switches it to the focus style.
    REQUIRE(imza::test::click_label(doc, "Revise Plan"));
    const std::string clicked
        = button_row(imza::test::to_text(doc->Render(), 100, 40));
    CHECK(clicked.find("\x1b[1m") != std::string::npos);
    CHECK(clicked.find("\x1b[48;2;45;50;56m") != std::string::npos);
}
TEST_CASE("revise guards are visible in the pane header")
{
    auto state = imza::test::make_test_state(
        imza::test::run_immediately, imza::test::test_config());
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    REQUIRE(doc->OnEvent(ftxui::Event::Character("s")));
    CHECK(state->session->error().find("note") != std::string::npos);
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("Add a note before sending.") != std::string::npos);
    CHECK(state->session->items().empty());
}

TEST_CASE("a second revise works after the agent updates the plan")
{
    imza::test::PostPump pump;
    auto state
        = imza::test::make_test_state(pump.fn(), imza::test::test_config(),
            [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
                cb(imza::make_delta_event("done"));
                cb(imza::make_done_event());
                return imza::Status::OK;
            });
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);

    // First revise.
    (void)doc->Render();
    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("first pass")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    (void)doc->Render();
    REQUIRE(doc->OnEvent(ftxui::Event::Character("s")));
    REQUIRE(pump.wait_for([&] { return imza::test::idle(*state->session); }));

    // The agent revises: a plan change lands.
    REQUIRE(state->session->create_plan(skeleton + "\nagent applied notes")
            .empty());
    (void)doc->Render();

    // Second revise starts a fresh turn.
    REQUIRE(doc->OnEvent(ftxui::Event::Character("c")));
    for (const char c : std::string("second pass")) {
        REQUIRE(doc->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
    REQUIRE(doc->OnEvent(ftxui::Event::Return));
    (void)doc->Render();
    REQUIRE(doc->OnEvent(ftxui::Event::Character("s")));
    bool found = false;
    for (const auto& item : state->session->items()) {
        const auto* user = std::get_if<imza::UserTurn>(&item);
        if (user != nullptr
            && user->text.find("second pass") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}
