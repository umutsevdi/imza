#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "runtime/mcp_subsystem.h"
#include "test_helpers.h"
#include "test_state.h"
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

    REQUIRE(imza::test::click_label(panel, "Build Notes - 1", 120, 40));
    const imza::ModalPayload payload = state->session->modal();
    const auto* viewer               = std::get_if<imza::ViewerModal>(&payload);
    REQUIRE(viewer != nullptr);
    CHECK(viewer->title == "Build Notes - 1");
    CHECK(viewer->content.find("chose A over B") != std::string::npos);
    CHECK(viewer->content.find("added dependency Z") != std::string::npos);
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

TEST_CASE("permissions box renders only custom settings and grants")
{
    const std::vector<imza::PermissionGrant> grants {
        imza::ExternalGrant { "/outside" },
        imza::ExternalGrant { "/outside/generated" },
        imza::SkillGrant { "/skills/docs/SKILL.md" },
        imza::ShellCommandGrant { "cmake", "--build" },
    };
    const auto flags
        = static_cast<imza::RuntimeFlag>(imza::SHELL | imza::ATTENDED);

    const imza::PermissionView view
        = imza::make_permission_view(flags, true, grants);
    const std::string out = to_text(imza::render_permissions_box(view));

    CHECK(imza::has_custom_permissions(view));
    CHECK(out.find("Permissions") != std::string::npos);
    CHECK(out.find("/outside") != std::string::npos);
    CHECK(out.find("cmake --build") != std::string::npos);

    // Default permissions contribute no box at all.
    const imza::PermissionView plain = imza::make_permission_view(
        imza::interactive_runtime_flags(), false, { });
    CHECK_FALSE(imza::has_custom_permissions(plain));
    CHECK(to_text(imza::render_permissions_box(plain)).find("Permissions")
        == std::string::npos);
}
