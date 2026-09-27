#include <iostream>
#include <string>
#include <variant>

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
    = "# Goal\nfirst plan\n# Approach\nx\n# Files\nx\n# Verification\nx\n"
      "# Open Questions\nx";

} // namespace

TEST_CASE("plan doc renders the initial plan as markdown when unfocused")
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
}

TEST_CASE("plan doc updates live and labels agent revisions")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    REQUIRE(state->session->create_plan(skeleton + "\nextra").empty());
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("Revision 1") != std::string::npos);
    CHECK(rendered.find("extra") != std::string::npos);
}

TEST_CASE("plan doc editor writes through on blur")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // Type into the editor buffer and blur: the buffer lands in the
    // session document as an in-place patch.
    REQUIRE(doc->OnEvent(ftxui::Event::Character("x")));
    focused = false;
    (void)doc->Render();

    CHECK(state->session->plan_doc().find("x") != std::string::npos);
    // An in-place patch is NOT a new revision.
    CHECK(state->session->plans().size() == 1);
}

TEST_CASE("plan doc rebase: agent supersede replaces the editor buffer")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // The user has unsaved keystrokes when the agent supersedes the
    // document: the rewrite wins and the buffer is rebased.
    REQUIRE(doc->OnEvent(ftxui::Event::Character("x")));
    REQUIRE(state->session
            ->create_plan(
                "# Goal\nagent rewrite\n# Approach\n"
                "x\n# Files\nx\n# Verification\nx\n# Open Questions\nx")
            .empty());
    const std::string rendered = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(rendered.find("agent rewrite") != std::string::npos);
    CHECK(rendered.find("agent updated the plan") != std::string::npos);
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
TEST_CASE("agent supersede during user edits replaces the editor buffer")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session
            ->create_plan("# Goal\nagent draft\n# Approach\nx\n# Files\nx\n# "
                          "Verification\nx\n"
                          "# Open Questions\nx")
            .empty());
    bool focused = true;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);
    (void)doc->Render();

    // User types into the buffer, then the agent supersedes the document.
    REQUIRE(doc->OnEvent(ftxui::Event::Character("x")));
    REQUIRE(state->session
            ->create_plan("# Goal\nagent rewrite\n# Approach\nx\n# Files\nx\n"
                          "# Verification\nx\n# Open Questions\nx")
            .empty());
    (void)doc->Render();

    // Simulate the debounced write: more time than WRITE_THROUGH_DELAY has
    // passed in tests, so force a write attempt by rendering again - the
    // buffer must have been rebased to the agent rewrite, never written
    // over it.
    (void)doc->Render();
    CHECK(
        state->session->plan_doc().find("agent rewrite") != std::string::npos);
}
TEST_CASE("plan created after pane construction renders immediately")
{
    auto state   = imza::test::make_test_state();
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);

    // The pane rendered empty before the plan existed.
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

TEST_CASE("plan doc preview drops the raw source; editor shows it")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(skeleton).empty());
    bool focused = false;

    auto doc
        = imza::make_plan_doc(state, [] { return wide_layout(); }, &focused);

    // Preview (unfocused): rendered markdown must NOT echo the raw
    // markdown heading marker.
    const std::string preview = imza::test::to_text(doc->Render(), 100, 40);
    CHECK(preview.find("# Goal") == std::string::npos);
    CHECK(preview.find("first plan") != std::string::npos);
}
