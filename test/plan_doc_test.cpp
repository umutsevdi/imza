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

// Taller document with a unique last line for scroll-follow tests; the
// long paragraph wraps like real plan documents.
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

// The pane reads `focused` by address, so it must outlive the component.
FocusedDoc make_focused_doc(std::shared_ptr<imza::ApplicationState> state,
    bool focused, const std::string& plan = { })
{
    imza::test::wait_for_environment(*state);
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

// Four rows down from the top: heading Requirements, first plan, heading
// Approach, first item, second item.
void note_second_item(FocusedDoc& fx, std::string_view note)
{
    for (int i = 0; i < 4; ++i) {
        REQUIRE(fx.doc->OnEvent(ftxui::Event::ArrowDown));
    }
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, note);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
}

// Mirrors PlanTab::OnRender: the pane is nested in an hbox.
std::string tab_frame(FocusedDoc& fx)
{
    return imza::test::to_text(
        ftxui::hbox({
            ftxui::text("chat") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30),
            ftxui::separatorEmpty(),
            fx.doc->Render() | ftxui::xflex,
        }),
        60, 9);
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
    CHECK(rendered.find("Plan Revision - 1") != std::string::npos);
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

    note_second_item(fx, "split this step");

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

    note_second_item(fx, "reorder these");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));

    // A provider is configured, so the revise turn starts immediately with
    // the derived locators and the note body.
    const auto items = fx.state->session->items();
    REQUIRE(items->size() >= 1);
    const auto* user = std::get_if<imza::UserTurn>(&items->front());
    REQUIRE(user != nullptr);
    CHECK(user->text.find("Approach > 2. second step") != std::string::npos);
    CHECK(user->text.find("reorder these") != std::string::npos);
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

    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("e")));
    imza::test::require_type(fx.doc, " plus");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    CHECK(
        imza::test::to_text(fx.doc->Render(), 100, 40).find("first draft plus")
        != std::string::npos);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::Delete));
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

    // ] jumps to the next section heading; a note on the heading row anchors
    // the whole section without a block excerpt.
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("]")));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "too detailed");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));

    const auto items = fx.state->session->items();
    REQUIRE(items->size() >= 1);
    const auto* user = std::get_if<imza::UserTurn>(&items->front());
    REQUIRE(user != nullptr);
    // The locator cites the section with the heading line as its anchor.
    CHECK(user->text.find("`Approach > # Approach`") != std::string::npos);
    CHECK(user->text.find("too detailed") != std::string::npos);
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
    // Every rendered row stays inside the pane width.
    for (const std::string& line : imza::split_lines(rendered)) {
        const std::string visible = imza::test::without_ansi(line);
        std::size_t end           = visible.size();
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

    REQUIRE(imza::test::click_label(fx.doc, "first plan"));
    CHECK(imza::test::to_text(fx.doc->Render(), 100, 40).find("Leave a note")
        != std::string::npos);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::Escape));
    const std::string closed = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(closed.find("Leave a note") == std::string::npos);
}

TEST_CASE("click on a note card selects it for edit and delete")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, skeleton);
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(imza::test::click_label(fx.doc, "first plan"));
    imza::test::require_type(fx.doc, "annotate this");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);

    // Clicking elsewhere selects a block row and opens the editor; closing
    // it leaves the cursor off the card, so e has nothing to edit.
    REQUIRE(imza::test::click_label(fx.doc, "Verification"));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Escape));
    REQUIRE_FALSE(fx.doc->OnEvent(ftxui::Event::Character("e")));

    REQUIRE(imza::test::click_label(fx.doc, "annotate this"));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("e")));
    imza::test::require_type(fx.doc, " more");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    const std::string rendered = imza::test::to_text(fx.doc->Render(), 100, 40);
    CHECK(rendered.find("annotate this more") != std::string::npos);

    REQUIRE(imza::test::click_label(fx.doc, "annotate this"));
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Delete));
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
    CHECK(fx.state->session->items()->empty());
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
    const auto items = fx.state->session->items();
    REQUIRE(items->size() >= 1);
    const auto* user = std::get_if<imza::UserTurn>(&items->front());
    REQUIRE(user != nullptr);
    CHECK(user->text.find("tighten scope") != std::string::npos);
}

TEST_CASE("revise works repeatedly with and without a plan change")
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

    const auto submitted = [&fx](std::string_view needle) {
        for (const auto& item : *fx.state->session->items()) {
            const auto* user = std::get_if<imza::UserTurn>(&item);
            if (user != nullptr
                && user->text.find(needle) != std::string::npos) {
                return true;
            }
        }
        for (const auto& queued : *fx.state->session->queued()) {
            if (queued.text.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "first pass");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));
    REQUIRE(
        pump.wait_for([&] { return imza::test::idle(*fx.state->session); }));

    // The agent answers without touching the plan; a second note and the
    // button must still submit. The pane re-renders between keystrokes in the
    // app, so mirror that here.
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "second pass");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(imza::test::click_label(fx.doc, "Revise Plan"));
    CHECK(submitted("second pass"));
    REQUIRE(
        pump.wait_for([&] { return imza::test::idle(*fx.state->session); }));

    // A plan change between turns must not block the next revise either.
    REQUIRE(fx.state->session->create_plan(skeleton + "\nagent applied notes")
            .empty());
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    imza::test::require_type(fx.doc, "third pass");
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Return));
    (void)imza::test::to_text(fx.doc->Render(), 100, 40);
    REQUIRE(fx.doc->OnEvent(ftxui::Event::Character("s")));
    CHECK(submitted("third pass"));
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
    CHECK(fx.state->session->items()->empty());
}

TEST_CASE("keyboard follow scrolls the window inside the tab layout")
{
    auto fx = make_focused_doc(imza::test::make_test_state(), true, tall);
    const auto frame = [&] { return tab_frame(fx); };

    const std::string head = frame();
    (void)frame();
    CHECK(head.find("Requirements") != std::string::npos);

    REQUIRE(fx.doc->OnEvent(ftxui::Event::End));
    const std::string tail = frame();
    CHECK(tail.find("tail marker zzz") != std::string::npos);
    CHECK(tail.find("Requirements") == std::string::npos);

    // The selected row's text stays on screen at every step.
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
    const auto frame = [&] { return tab_frame(fx); };
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
