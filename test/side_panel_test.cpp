#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "test_helpers.h"
#include "test_state.h"
#include "tools/mcp_manager.h"
#include "ui/ui.h"
#include "workspace/environment.h"

using imza::test::to_screen;
using imza::test::to_text;

TEST_CASE("side panel documents widget hides when empty and lists plans")
{
    auto state    = imza::test::make_test_state();
    int navigated = 0;
    auto panel    = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; },
        [&navigated](imza::WorkflowPhase) { ++navigated; });

    const std::string empty = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(empty.find("Documents") == std::string::npos);

    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON + "\nmore")
            .empty());

    const std::string out = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(out.find("Documents") != std::string::npos);
    CHECK(out.find("Initial Plan") != std::string::npos);
    CHECK(out.find("Plan Revision - 1") != std::string::npos);
}

TEST_CASE("documents widget interleaves build logs with plan revisions")
{
    auto state = imza::test::make_test_state();
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());
    REQUIRE(state->session->append_build_decision("chose A over B").empty());
    REQUIRE(
        state->session->append_build_decision("added dependency Z").empty());
    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON + "\nmore")
            .empty());
    REQUIRE(state->session->append_build_decision("second stint").empty());

    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; }, [](imza::WorkflowPhase) { });

    const std::string out = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(out.find("Initial Plan") != std::string::npos);
    CHECK(out.find("Build Notes - 1") != std::string::npos);
    CHECK(out.find("Plan Revision - 1") != std::string::npos);
    CHECK(out.find("Build Notes - 2") != std::string::npos);

    // Clicking a build row opens its log in the viewer.
    REQUIRE(imza::test::click_label(panel, "Build Notes - 1", 120, 40));
    const imza::ModalPayload payload = state->session->modal();
    const auto* viewer               = std::get_if<imza::ViewerModal>(&payload);
    REQUIRE(viewer != nullptr);
    CHECK(viewer->title == "Build Notes - 1");
    CHECK(viewer->content.find("chose A over B") != std::string::npos);
    CHECK(viewer->content.find("added dependency Z") != std::string::npos);
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

TEST_CASE("side panel hides widgets disabled in the sidebar config")
{
    imza::Config config      = imza::test::test_config();
    config.sidebar.documents = false;
    config.sidebar.tasks     = false;
    auto state
        = imza::test::make_test_state(imza::test::run_immediately, config);

    REQUIRE(state->session->create_plan(imza::test::PLAN_SKELETON).empty());
    state->session->set_todo(imza::TodoList {
        { { "hidden task", imza::TodoItem::Status::PENDING } } });

    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; }, [](imza::WorkflowPhase) { });

    const std::string out = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(out.find("Documents") == std::string::npos);
    CHECK(out.find("Initial Plan") == std::string::npos);
    CHECK(out.find("Tasks") == std::string::npos);
    CHECK(out.find("hidden task") == std::string::npos);
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

TEST_CASE("side panel context box lists only reachable mcp servers")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    REQUIRE(server.start());

    auto state = imza::test::make_test_state();
    state->mcp->reload({ { "reachable",
        imza::McpServerConfig { .id = "reachable", .url = server.url() } } });
    state->mcp->connect("reachable");
    imza::test::wait_until([&] {
        for (const auto& entry : state->mcp->snapshot()) {
            if (entry.id == "reachable"
                && entry.state == imza::McpServerState::CONNECTED) {
                return true;
            }
        }
        return false;
    });

    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; }, [](imza::WorkflowPhase) { });

    const std::string connected = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(connected.find("MCP") != std::string::npos);
    CHECK(connected.find("reachable") != std::string::npos);

    state->mcp->reload({ { "reachable",
                             imza::McpServerConfig {
                                 .id = "reachable", .url = server.url() } },
        { "dormant",
            imza::McpServerConfig { .id = "dormant", .url = server.url() } } });
    const std::string offline = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(offline.find("dormant") == std::string::npos);

    server.stop();
}

TEST_CASE("render_context_box lists attachment basenames under files")
{
    const std::string out = to_text(imza::render_context_box(
        "AGENTS.md", { "main.cpp", "design.md" }, { }, { }));

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
