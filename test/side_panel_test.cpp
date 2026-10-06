#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"
#include "workspace/environment.h"

using imza::test::to_screen;
using imza::test::to_text;

TEST_CASE("side panel plans widget hides when empty and lists plans")
{
    auto state    = imza::test::make_test_state();
    int navigated = 0;
    auto panel    = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; },
        [&navigated](imza::WorkflowPhase) { ++navigated; });

    const std::string empty = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(empty.find("Plans") == std::string::npos);

    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON + "\nmore")
            .empty());

    const std::string out = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(out.find("Plans") != std::string::npos);
    CHECK(out.find("Initial Plan") != std::string::npos);
    CHECK(out.find("Revision 1") != std::string::npos);
}

TEST_CASE("only the latest plan renders bright, older ones stay dim")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON + "\nmore")
            .empty());

    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; }, [](imza::WorkflowPhase) { });

    auto screen = imza::test::to_screen(panel->Render(), 120, 40);

    bool initial_is_dim  = false;
    bool revision_is_dim = false;
    bool initial_found   = false;
    bool revision_found  = false;
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row;
        for (int x = 0; x < screen.dimx(); ++x) {
            row += screen.at(x, y);
        }
        // Narrow panels squeeze word spaces out of wrapped paragraphs.
        if (row.find("Initial") != std::string::npos) {
            initial_found  = true;
            initial_is_dim = screen.CellAt(1, y).dim;
        }
        if (row.find("Revision") != std::string::npos) {
            revision_found  = true;
            revision_is_dim = screen.CellAt(1, y).dim;
        }
    }
    REQUIRE(initial_found);
    REQUIRE(revision_found);
    REQUIRE(initial_is_dim);
    REQUIRE_FALSE(revision_is_dim);
}

TEST_CASE("clicking a plan opens it in the viewer modal")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());

    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; }, [](imza::WorkflowPhase) { });

    REQUIRE(imza::test::click_label(panel, "Initial Plan", 120, 40));
    const imza::ModalPayload payload = state->session->modal();
    const auto* viewer               = std::get_if<imza::ViewerModal>(&payload);
    REQUIRE(viewer != nullptr);
    CHECK(viewer->title == "Initial Plan");
    CHECK(viewer->content == imza::test::PLAN_SKELETON);
}

TEST_CASE("render_todo renders status marks")
{
    using Status = imza::TodoItem::Status;
    imza::TodoList todo { { { "a", Status::PENDING },
        { "b", Status::IN_PROGRESS }, { "c", Status::COMPLETED } } };

    const std::string wide = to_text(
        imza::render_todo(todo, { imza::LayoutCtx::Kind::WIDE, 120 }));
    CHECK(wide.find("[ ]") != std::string::npos);
    CHECK(wide.find("→") != std::string::npos);
    CHECK(wide.find("[x]") != std::string::npos);
}

TEST_CASE("render_todo wraps long items instead of clipping")
{
    using Status = imza::TodoItem::Status;
    imza::TodoList todo {
        { { "investigate the flaky parser regression test", Status::PENDING } }
    };
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(30), ftxui::Dimension::Fixed(10));
    ftxui::Render(screen,
        imza::render_todo(todo, { imza::LayoutCtx::Kind::WIDE, 30 })
            | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30));
    const std::string out = screen.ToString();
    CHECK(out.find("investigate") != std::string::npos);
    CHECK(out.find("flaky") != std::string::npos);
    CHECK(out.find("regression") != std::string::npos);
    CHECK(out.find("test") != std::string::npos);
}

TEST_CASE("render_context_box lists attachment basenames under files")
{
    const std::string out = to_text(imza::render_context_box(
        "AGENTS.md", { "main.cpp", "design.md" }, { }, { }, { }));

    CHECK(out.find("Files") != std::string::npos);
    CHECK(out.find("AGENTS.md") != std::string::npos);
    CHECK(out.find("main.cpp") != std::string::npos);
    CHECK(out.find("design.md") != std::string::npos);
}

TEST_CASE("permissions box stays hidden for default permissions")
{
    const imza::PermissionView view
        = imza::make_permission_view(imza::interactive_runtime_flags(), { });

    CHECK_FALSE(imza::has_custom_permissions(view));
    CHECK(to_text(imza::render_permissions_box(view)).find("Permissions")
        == std::string::npos);
}

TEST_CASE("permissions box renders only custom settings and grants")
{
    const std::vector<imza::PermissionGrant> grants {
        imza::ExternalGrant { "/outside" },
        imza::ExternalGrant { "/outside/generated" },
        imza::SkillGrant { "/skills/docs/SKILL.md" },
        imza::ShellCommandGrant { "cmake", "--build" },
    };
    const auto flags = static_cast<imza::RuntimeFlag>(
        imza::SHELL | imza::ATTENDED | imza::SKIP_PERMISSIONS);

    const imza::PermissionView view = imza::make_permission_view(flags, grants);
    const std::string out = to_text(imza::render_permissions_box(view));

    CHECK(imza::has_custom_permissions(view));
    CHECK(out.find("Permissions") != std::string::npos);
    CHECK(out.find("Web") != std::string::npos);
    CHECK(out.find("disabled") != std::string::npos);
    CHECK(out.find("Approvals") != std::string::npos);
    CHECK(out.find("skipped") != std::string::npos);
    CHECK(out.find("Shell") == std::string::npos);
    CHECK(out.find("Files") == std::string::npos);
    CHECK(out.find("/outside") != std::string::npos);
    CHECK(out.find("/outside/generated") != std::string::npos);
    CHECK(out.find("read ") == std::string::npos);
    CHECK(out.find("write ") == std::string::npos);
    CHECK(out.find("Skills") == std::string::npos);
    CHECK(out.find("docs") == std::string::npos);
    CHECK(out.find("cmake --build") != std::string::npos);
}
