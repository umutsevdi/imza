#include <string>
#include <utility>

#include <doctest/doctest.h>

#include <ftxui/component/event.hpp>

#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

namespace {

const std::string skeleton
    = "# Requirements\nfirst plan\n# Approach\n1. first step\n2. second step\n"
      "# Changes\nx\n# Verification\ncheck it";

// Taller document with a unique last line for scroll-follow tests;
// long paragraphs wrap, like real plan documents.
const std::string wrapped(200, 'w');
const std::string tall
    = "# Requirements\nfirst plan\n# Approach\n1. first step\n2. second step\n"
      "# Changes\nx\n# Verification\ncheck it\n"
    + wrapped + "\n# Notes\ntail marker zzz";

struct FocusedDoc {
    std::shared_ptr<imza::ApplicationState> state;
    ftxui::Component doc;
    bool focused = false;
};

// Build the annotator pane over `state`, seeding the plan first when one
// is given; the pane reads `focused` by address for its lifetime.
FocusedDoc make_focused_doc(std::shared_ptr<imza::ApplicationState> state,
    bool focused, const std::string& plan = { })
{
    if (!plan.empty()) {
        REQUIRE(state->session->create_plan(plan).empty());
    }
    FocusedDoc doc;
    doc.state   = std::move(state);
    doc.focused = focused;
    doc.doc     = imza::make_plan_doc(
        doc.state, [] { return imza::test::wide_layout(); }, &doc.focused);
    return doc;
}

} // namespace

TEST_CASE("plan doc renders the initial plan as block rows when unfocused")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), false, skeleton);

    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("Plan") != std::string::npos);
    CHECK(rendered.find("Initial Plan") != std::string::npos);
    CHECK(rendered.find("first plan") != std::string::npos);
    CHECK(rendered.find("second step") != std::string::npos);
}

TEST_CASE("plan doc updates live and labels agent revisions")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), false, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    REQUIRE(
        fx.state->session->create_plan(skeleton + "\nextra note from the agent")
            .empty());
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("Revision 1") != std::string::npos);
    CHECK(rendered.find("extra note from the agent") != std::string::npos);
}

TEST_CASE("note editor opens and saves on Enter")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    const std::string editor = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(editor.find("Leave a note") != std::string::npos);

    imza::test::require_type(fx.doc, "make it faster");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));

    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("make it faster") != std::string::npos);
    CHECK(rendered.find("1 notes") != std::string::npos);
}

TEST_CASE("note card renders under the annotated block")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    // Walk to the second list item: heading Requirements, first plan,
    // heading Approach, first item, second item.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(fx.doc->OnEvent(ftxui::Event::ArrowDown));
    }
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "split this step");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));

    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    const auto note_at         = rendered.find("split this step");
    const auto item_at         = rendered.find("second step");
    REQUIRE(note_at != std::string::npos);
    REQUIRE(item_at != std::string::npos);
    CHECK(note_at > item_at);
}

TEST_CASE("revise submits one turn with section and line locators")
{
    auto fx = make_focused_doc(
        imza::test::make_test_state(
            imza::test::run_immediately, imza::test::test_config()),
        true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    // Walk to the second list item: heading Requirements, first plan,
    // heading Approach, first item, second item.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(fx.doc->OnEvent(ftxui::Event::ArrowDown));
    }
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "reorder these");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));

    // The revise turn starts immediately (a provider is configured); the
    // user turn carries the derived locators and the note body.
    REQUIRE(fx.state->session->items().size() >= 1);
    const auto* user
        = std::get_if<imza::UserTurn>(&fx.state->session->items().front());
    REQUIRE(user != nullptr);
    CHECK(user->text.find("Approach > 2. second step") != std::string::npos);
    CHECK(user->text.find("reorder these") != std::string::npos);
    // Notes clear after the revise turn is sent.
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("reorder these") == std::string::npos);
}

TEST_CASE("unfocused pane ignores annotator keys and opens no editor")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), false, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    CHECK_FALSE(fx.doc->OnEvent(ftxui::Event::Return));
    CHECK_FALSE(fx.doc->OnEvent(ftxui::Event::Character("s")));
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("Leave a note") == std::string::npos);
    CHECK(rendered.find("first plan") != std::string::npos);
}

TEST_CASE("e and d edit and delete the selected note")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "first draft");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));

    // The cursor parks on the saved note card after the next render;
    // edit it.
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("e")));
    imza::test::require_type(fx.doc, " plus");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    CHECK(
        imza::test::to_text(fx.doc->Render(), 100, 40).find("first draft plus")
        != std::string::npos);

    // Delete it from the note row.
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("d")));
    CHECK(imza::test::to_text(fx.doc->Render(), 100, 40).find("first draft")
        == std::string::npos);
}

TEST_CASE("section jumps with brackets and a heading note anchors the section")
{
    auto fx = make_focused_doc(
        imza::test::make_test_state(
            imza::test::run_immediately, imza::test::test_config()),
        true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    // ] jumps to the next section heading (Approach); a note on the
    // heading row anchors the whole section without a block excerpt.
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("]")));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "too detailed");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));

    REQUIRE(fx.state->session->items().size() >= 1);
    const auto* user
        = std::get_if<imza::UserTurn>(&fx.state->session->items().front());
    REQUIRE(user != nullptr);
    // The heading note pins the heading line; the locator cites the
    // section with the heading line itself as the edit anchor.
    CHECK(user->text.find("`Approach > # Approach`") != std::string::npos);
    CHECK(user->text.find("too detailed") != std::string::npos);
}

TEST_CASE("plan doc without a plan shows the empty hint")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), false);
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("No plan") != std::string::npos);
}

TEST_CASE("plan created after pane construction renders immediately")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), false);

    const std::string empty = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(empty.find("No plan") != std::string::npos);

    REQUIRE(fx.state->session
            ->create_plan("# Requirements\nlate draft\n# Approach\nx\n# "
                          "Changes\nx\n# Verification\nx")
            .empty());
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("late draft") != std::string::npos);
    CHECK(rendered.find("Initial Plan") != std::string::npos);
}

TEST_CASE("long document lines wrap inside the pane instead of overflowing")
{
    auto state                  = imza::test::make_test_state();
    const std::string long_line = "# Requirements\n" + std::string(300, 'x')
        + "\n# Approach\nx\n# Changes\nx\n# Verification\nx";
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
    auto fx = make_focused_doc(imza::test::make_test_state(), true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    // Click the "first plan" block row.
    REQUIRE(imza::test::click_label(fx.doc, "first plan"));

    // The editor card renders in-flow between the clicked block and the
    // next block row, not at the bottom bar.
    const std::vector<std::string> lines
        = imza::split_lines(imza::test::to_text(fx.doc->Render(), 100, 40));
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
    CHECK(imza::test::to_text(fx.doc->Render(), 100, 40).find("Revise Plan")
        != std::string::npos);

    // Escape closes the editor; typing and Enter still save.
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Escape));
    const std::string closed = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(closed.find("Leave a note") == std::string::npos);
}

TEST_CASE("moving with keys highlights the selected row")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

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

    // Row 0 is selected: the "Requirements" heading row is highlighted.
    CHECK(highlight_at(
              imza::test::to_text(fx.doc->Render(), 100, 40), "Requirements")
        != -1);

    // Move down twice: row 2 is the "Approach" heading; it is highlighted
    // and "Requirements" is not.
    REQUIRE(fx.doc->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::ArrowDown));
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(highlight_at(rendered, "Approach") != -1);
    CHECK(highlight_at(rendered, "Requirements") == -1);
}

TEST_CASE("click on a note card selects it for edit and delete")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(imza::test::click_label(fx.doc, "first plan"));
    imza::test::require_type(fx.doc, "annotate this");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    // Clicking elsewhere selects a block row (and opens the editor);
    // closing it leaves the cursor off the card, so e has nothing to edit.
    REQUIRE(imza::test::click_label(fx.doc, "Verification"));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Escape));
    REQUIRE_FALSE(fx.doc->OnEvent(ftxui::Event::Character("e")));

    // Clicking the card selects it; e focuses the editor with the body.
    REQUIRE(imza::test::click_label(fx.doc, "annotate this"));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("e")));
    imza::test::require_type(fx.doc, " more");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("annotate this more") != std::string::npos);

    REQUIRE(imza::test::click_label(fx.doc, "annotate this"));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("d")));
    CHECK(imza::test::to_text(fx.doc->Render(), 100, 40).find("annotate this")
        == std::string::npos);
}

TEST_CASE("space opens a note instead of triggering revise")
{
    auto fx = make_focused_doc(
        imza::test::make_test_state(
            imza::test::run_immediately, imza::test::test_config()),
        true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character(" ")));
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("Leave a note") != std::string::npos);
    // No revise turn was submitted.
    CHECK(fx.state->session->items().empty());
}

TEST_CASE("revise button click submits the notes")
{
    auto fx = make_focused_doc(
        imza::test::make_test_state(
            imza::test::run_immediately, imza::test::test_config()),
        true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "tighten scope");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    REQUIRE(imza::test::click_label(fx.doc, "Revise Plan"));
    REQUIRE(fx.state->session->items().size() >= 1);
    const auto* user
        = std::get_if<imza::UserTurn>(&fx.state->session->items().front());
    REQUIRE(user != nullptr);
    CHECK(user->text.find("tighten scope") != std::string::npos);
}

TEST_CASE("revise works repeatedly without a plan change in between")
{
    auto fx = make_focused_doc(
        imza::test::make_test_state(
            imza::test::run_immediately, imza::test::test_config()),
        true, skeleton);

    // First revise via the keyboard.
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "first pass");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));
    REQUIRE(fx.state->session->items().size() >= 1);

    // The agent answers without touching the plan; a second note and the
    // button must still submit (no revise latch). The pane re-renders
    // between keystrokes in the app, so mirror that here.
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "second pass");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(imza::test::click_label(fx.doc, "Revise Plan"));
    bool found = false;
    for (const auto& item : fx.state->session->items()) {
        const auto* user = std::get_if<imza::UserTurn>(&item);
        if (user != nullptr
            && user->text.find("second pass") != std::string::npos) {
            found = true;
        }
    }
    // A busy session queues the revise instead of dropping it.
    for (const auto& queued : fx.state->session->queued()) {
        if (queued.text.find("second pass") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("revise button highlights on click like review buttons")
{
    auto fx = make_focused_doc(
        imza::test::make_test_state(
            imza::test::run_immediately, imza::test::test_config()),
        true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

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
        = button_row(imza::test::to_text(fx.doc->Render(), 100, 40));
    // Rest: calm background (not the focus panel), not bold.
    CHECK(rest.find("\x1b[48;2;72;79;88m") != std::string::npos);
    CHECK(rest.find("\x1b[1m") == std::string::npos);

    // Clicking the button submits and switches it to the focus style.
    REQUIRE(imza::test::click_label(fx.doc, "Revise Plan"));
    const std::string clicked
        = button_row(imza::test::to_text(fx.doc->Render(), 100, 40));
    CHECK(clicked.find("\x1b[1m") != std::string::npos);
    CHECK(clicked.find("\x1b[48;2;45;50;56m") != std::string::npos);
}
TEST_CASE("revise guards are visible in the pane header")
{
    auto fx = make_focused_doc(
        imza::test::make_test_state(
            imza::test::run_immediately, imza::test::test_config()),
        true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));
    CHECK(fx.state->session->error().find("note") != std::string::npos);
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("Add a note before sending.") != std::string::npos);
    CHECK(fx.state->session->items().empty());
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
    auto fx = make_focused_doc(state, true, skeleton);

    // First revise.
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "first pass");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));
    REQUIRE(
        pump.wait_for([&] { return imza::test::idle(*fx.state->session); }));

    // The agent revises: a plan change lands.
    REQUIRE(fx.state->session->create_plan(skeleton + "\nagent applied notes")
            .empty());
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    // Second revise starts a fresh turn.
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "second pass");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));
    bool found = false;
    for (const auto& item : fx.state->session->items()) {
        const auto* user = std::get_if<imza::UserTurn>(&item);
        if (user != nullptr
            && user->text.find("second pass") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}
TEST_CASE("keyboard follow scrolls the window inside the tab layout")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, tall);

    // Mimic PlanTab::OnRender: the pane is nested inside an hbox and
    // the element tree is rebuilt on every frame.
    const auto frame = [&] {
        return imza::test::to_text(
            ftxui::hbox({
                ftxui::text("chat")
                    | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30),
                ftxui::separatorEmpty(),
                fx.doc->Render() | ftxui::xflex,
            }),
            60, 9);
    };

    const std::string head = frame();
    (void)frame();
    CHECK(head.find("Requirements") != std::string::npos);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::End));
    const std::string tail = frame();
    CHECK(tail.find("tail marker zzz") != std::string::npos);
    CHECK(tail.find("Requirements") == std::string::npos);

    // Walking down row by row from the top: the selected row's text
    // must stay on screen at every step.
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Home));
    (void)frame();
    const std::string wrapped_prefix(20, 'w');
    const std::vector<std::string> row_text = { "Requirements", "first plan",
        "Approach", "first step", "second step", "Changes", "Verification",
        "check it", wrapped_prefix, "Notes", "tail marker zzz" };
    for (int i = 0; i < static_cast<int>(row_text.size()) - 1; ++i) {
        REQUIRE(fx.doc->OnEvent(ftxui::Event::ArrowDown));
        const std::string window = frame();
        CHECK(window.find(row_text[static_cast<std::size_t>(i + 1)])
            != std::string::npos);
    }
    CHECK(frame().find("tail marker zzz") != std::string::npos);
}

TEST_CASE("wheel scroll moves the selection like review")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, tall);
    const auto frame = [&] {
        return imza::test::to_text(
            ftxui::hbox({
                ftxui::text("chat")
                    | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30),
                ftxui::separatorEmpty(),
                fx.doc->Render() | ftxui::xflex,
            }),
            60, 9);
    };
    const auto wheel = [](ftxui::Mouse::Button button) {
        ftxui::Mouse mouse;
        mouse.button = button;
        mouse.motion = ftxui::Mouse::Pressed;
        mouse.x      = 50;
        mouse.y      = 4;
        return ftxui::Event::Mouse("", mouse);
    };

    (void)frame();
    for (int i = 0; i < 10; ++i) {
        REQUIRE(fx.doc->OnEvent(wheel(ftxui::Mouse::WheelDown)));
        (void)frame();
    }
    CHECK(imza::test::without_ansi(frame()).find("tail marker zzz")
        != std::string::npos);

    for (int i = 0; i < 10; ++i) {
        REQUIRE(fx.doc->OnEvent(wheel(ftxui::Mouse::WheelUp)));
    }
    const std::string head = frame();
    CHECK(head.find("Requirements") != std::string::npos);
}
