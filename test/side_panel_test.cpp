#include <filesystem>
#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "runtime/mcp_subsystem.h"
#include "test_fs.h"
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

TEST_CASE("runtime widget counts only reachable mcp servers")
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
    CHECK(connected.find("1/1") != std::string::npos);
    CHECK(connected.find("reachable") == std::string::npos);

    REQUIRE(imza::test::click_label(panel, "MCP", 120, 40));
    const std::string expanded = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(expanded.find("reachable") != std::string::npos);

    // A server that is configured but not connected is not counted.
    state->mcp->reload({ { "reachable",
                             imza::McpServerConfig {
                                 .id = "reachable", .url = server.url() } },
        { "dormant",
            imza::McpServerConfig { .id = "dormant", .url = server.url() } } });
    const std::string offline = imza::test::to_text(panel->Render(), 120, 40);
    CHECK(offline.find("dormant") == std::string::npos);

    server.stop();
}

TEST_CASE("permission view keeps skill grants alongside the rest")
{
    const std::vector<imza::PermissionGrant> grants {
        imza::ExternalGrant { "/outside" },
        imza::SkillGrant { "/skills/docs/SKILL.md" },
        imza::ShellCommandGrant { "cmake", "--build" },
    };
    const auto flags
        = static_cast<imza::RuntimeFlag>(imza::SHELL | imza::ATTENDED);

    const imza::PermissionView view
        = imza::make_permission_view(flags, true, grants);

    CHECK(imza::has_custom_permissions(view));
    CHECK(view.folders.size() == 1);
    CHECK(view.commands.size() == 1);
    CHECK(view.skills.size() == 1);
    CHECK(view.approvals_skipped);

    const imza::PermissionView plain = imza::make_permission_view(
        imza::interactive_runtime_flags(), false, { });
    CHECK_FALSE(imza::has_custom_permissions(plain));
}

namespace {

imza::RuntimeView runtime_view_with_grants()
{
    imza::RuntimeView view;
    view.rules         = "/project/AGENTS.md";
    view.attachments   = { "src/main.cpp", "docs/spec.md" };
    view.skills        = { { "caveman", true }, { "anatomy", false } };
    view.mcp_connected = { { "exa", "4 tools" }, { "github", "12 tools" } };
    view.mcp_total     = 4;
    view.permissions   = imza::make_permission_view(
        static_cast<imza::RuntimeFlag>(imza::SHELL | imza::ATTENDED), false,
        { imza::ExternalGrant { "/outside" },
            imza::ShellCommandGrant { "cmake", "--build" } });
    return view;
}

imza::RuntimeRowLink plain_row_link()
{
    return
        [](imza::RuntimeRow, std::string_view label, std::string_view value) {
            return imza::hover_link(
                [text = std::string(label) + " " + std::string(value)] {
                    return ftxui::text(text);
                },
                [] { });
        };
}

std::size_t count_of(std::string_view haystack, std::string_view needle)
{
    std::size_t hits = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
        at              = haystack.find(needle, at + needle.size())) {
        ++hits;
    }
    return hits;
}

} // namespace

TEST_CASE("files row counts the context files and repeats none")
{
    imza::RuntimeView view;
    view.rules       = "/project/AGENTS.md";
    view.attachments = { "AGENTS.md", "src/main.cpp" };

    const std::string out
        = imza::test::without_ansi(to_text(imza::render_runtime_box(
            view, { imza::RuntimeRow::FILES }, plain_row_link())));

    // The attachment repeating the rules file is dropped, not listed twice.
    CHECK(count_of(out, "AGENTS.md") == 1);
    CHECK(count_of(out, "src/main.cpp") == 1);
}

TEST_CASE("runtime box lists entries only for expanded rows")
{
    const imza::RuntimeView view = runtime_view_with_grants();

    const std::string collapsed = imza::test::without_ansi(
        to_text(imza::render_runtime_box(view, { }, plain_row_link()), 40, 20));
    CHECK(collapsed.find("caveman") == std::string::npos);
    CHECK(collapsed.find("exa") == std::string::npos);

    const std::string expanded = imza::test::without_ansi(
        to_text(imza::render_runtime_box(view,
                    { imza::RuntimeRow::SKILLS, imza::RuntimeRow::MCP },
                    plain_row_link()),
            40, 20));
    CHECK(expanded.find("caveman") != std::string::npos);
    CHECK(expanded.find("exa") != std::string::npos);
    CHECK(expanded.find("src/main.cpp") == std::string::npos);
}

TEST_CASE("runtime box hides empty rows and vanishes when nothing is set")
{
    imza::RuntimeView view;
    view.rules = "/project/AGENTS.md";
    const std::string files_only
        = to_text(imza::render_runtime_box(view, { }, plain_row_link()));
    CHECK(files_only.find("Files") != std::string::npos);
    CHECK(files_only.find("Skills") == std::string::npos);
    CHECK(files_only.find("MCP") == std::string::npos);

    const imza::RuntimeView empty;
    CHECK_FALSE(imza::has_runtime_content(empty));
    CHECK(to_text(imza::render_runtime_box(empty, { }, plain_row_link()))
              .find("Runtime")
        == std::string::npos);
}

TEST_CASE("runtime permissions markdown reports capabilities and grants")
{
    const imza::RuntimeView view = runtime_view_with_grants();
    const std::string out        = imza::runtime_permissions_markdown(view);

    CHECK(out.find("| Web | off |") != std::string::npos);
    CHECK(out.find("| Shell | on |") != std::string::npos);
    CHECK(out.find("/outside") != std::string::npos);
    CHECK(out.find("cmake --build") != std::string::npos);
    // The viewer renders the title; the body must not repeat it.
    CHECK(out.find("# Permissions") == std::string::npos);
    // Approvals are only listed when bypassed.
    CHECK(out.find("Approvals") == std::string::npos);

    imza::RuntimeView skipped             = view;
    skipped.permissions.approvals_skipped = true;
    CHECK(imza::runtime_permissions_markdown(skipped).find(
              "| Approvals | **skipped** |")
        != std::string::npos);
}

TEST_CASE("clicking the permissions row opens a read-only viewer modal")
{
    imza::test::TempDir dir;
    const std::filesystem::path outside = dir.file("outside");
    std::filesystem::create_directories(outside);

    auto state = imza::test::make_test_state();
    REQUIRE(state->permissions->install({ imza::ExternalGrant { outside } }));

    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::PLAN; }, [](imza::WorkflowPhase) { });

    REQUIRE(imza::test::click_label(panel, "Permissions", 120, 40));
    const imza::ModalPayload payload = state->session->modal();
    const auto* viewer               = std::get_if<imza::ViewerModal>(&payload);
    REQUIRE(viewer != nullptr);
    CHECK(viewer->title == "Permissions");
    CHECK(viewer->lang == "md");
    CHECK(viewer->content.find(outside.string()) != std::string::npos);
}
