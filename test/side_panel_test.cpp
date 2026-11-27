#include <algorithm>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "platform/command_runner.h"
#include "runtime/mcp_subsystem.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_platform.h"
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

    const imza::PermissionView capped = imza::make_permission_view(
        imza::interactive_runtime_flags(), false, { }, 2);
    CHECK(capped.subagent_limit == 2);
    CHECK(imza::has_custom_permissions(capped));
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

namespace {

// One staged and one unstaged change, so the widget has both groups.
void seed_staged_and_unstaged(const std::filesystem::path& root)
{
    imza::test::init_test_repo(root, true);
    const auto git = [&root](const std::string& arguments) {
        REQUIRE(imza::command_ok(imza::run_command(
            imza::git_command(root, arguments), std::chrono::seconds { 10 },
            { }, { }, imza::StderrCapture::STDOUT_ONLY)));
    };
    imza::test::write_file(root / "staged.txt", "staged\n");
    imza::test::write_file(root / "unstaged.txt", "unstaged\n");
    git("add staged.txt");
}

std::shared_ptr<imza::ApplicationState> repo_state(
    const std::filesystem::path& root)
{
    const imza::test::CurrentDirectory keep;
    auto state = imza::test::make_test_state();
    std::filesystem::current_path(root);
    REQUIRE(state->environment->chdir(root)
        == imza::Environment::ChdirResult::CHANGED);
    imza::test::wait_for_environment(*state);
    REQUIRE(imza::test::wait_until([&] {
        const auto repository = state->environment->repository();
        return repository && repository->changed_files.size() == 2;
    }));
    std::filesystem::current_path(keep.original);
    return state;
}

} // namespace

TEST_CASE("changed files widget groups staged before unstaged with checkboxes")
{
    if (!imza::test::git_available()) {
        return;
    }
    const imza::test::TempDir scratch;
    seed_staged_and_unstaged(scratch.path);
    auto state = repo_state(scratch.path);
    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::BUILD; }, [](imza::WorkflowPhase) { });

    const std::string out
        = imza::test::without_ansi(to_text(panel->Render(), 120, 40));
    const std::size_t staged_at   = out.find("staged.txt");
    const std::size_t divider_at  = out.find("Unstaged");
    const std::size_t unstaged_at = out.find("unstaged.txt");
    const std::size_t commit_at   = out.rfind("Commit");

    REQUIRE(staged_at != std::string::npos);
    REQUIRE(divider_at != std::string::npos);
    REQUIRE(unstaged_at != std::string::npos);
    REQUIRE(commit_at != std::string::npos);
    CHECK(staged_at < divider_at);
    CHECK(divider_at < unstaged_at);
    CHECK(unstaged_at < commit_at);
    // The staged row shows a checked box, the unstaged row an empty one;
    // the glyphs are ftxui's own Checkbox marks.
    const std::string staged_row = out.substr(0, staged_at);
    CHECK(staged_row.rfind("▣") != std::string::npos);
    const std::string unstaged_row
        = out.substr(staged_at, unstaged_at - staged_at);
    CHECK(unstaged_row.rfind("☐") != std::string::npos);
    CHECK(unstaged_row.find("▣") == std::string::npos);
}

TEST_CASE("changed files widget omits the divider with only unstaged files")
{
    if (!imza::test::git_available()) {
        return;
    }
    const imza::test::TempDir scratch;
    seed_staged_and_unstaged(scratch.path);
    const auto unstaged = imza::run_command("git -C "
            + imza::shell_quote(scratch.path) + " restore --staged staged.txt",
        std::chrono::seconds { 10 });
    REQUIRE(imza::command_ok(unstaged));
    auto state = repo_state(scratch.path);
    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::BUILD; }, [](imza::WorkflowPhase) { });

    const std::string out
        = imza::test::without_ansi(to_text(panel->Render(), 120, 40));
    CHECK(out.find("Unstaged") == std::string::npos);
    CHECK(out.find("▣") == std::string::npos);
    CHECK(out.find("Commit") != std::string::npos);
}

TEST_CASE("sidebar Commit button opens the commit dialog")
{
    if (!imza::test::git_available()) {
        return;
    }
    const imza::test::TempDir scratch;
    seed_staged_and_unstaged(scratch.path);
    auto state = repo_state(scratch.path);
    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::BUILD; }, [](imza::WorkflowPhase) { });

    REQUIRE(imza::test::click_label(panel, "Commit", 120, 40));
    CHECK(std::holds_alternative<imza::CommitModal>(state->session->modal()));
}

TEST_CASE("sidebar bulk button stages all, then unstages all")
{
    if (!imza::test::git_available()) {
        return;
    }
    const imza::test::TempDir scratch;
    seed_staged_and_unstaged(scratch.path);
    auto state = repo_state(scratch.path);
    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::BUILD; }, [](imza::WorkflowPhase) { });

    REQUIRE(imza::test::click_label(panel, "Stage All", 120, 40));
    REQUIRE(imza::test::wait_until([&] {
        const auto repository = state->environment->repository();
        return repository
            && std::ranges::all_of(
                repository->changed_files, [](const imza::ChangedFile& file) {
                    return file.area == imza::ChangedFile::Area::STAGED;
                });
    }));
    CHECK(state->session->error().empty());

    // Everything staged: the same control now offers the reverse.
    const std::string staged_view
        = imza::test::without_ansi(to_text(panel->Render(), 120, 40));
    CHECK(staged_view.find("Unstage All") != std::string::npos);
    CHECK(staged_view.find("Stage All") == std::string::npos);

    REQUIRE(imza::test::click_label(panel, "Unstage All", 120, 40));
    REQUIRE(imza::test::wait_until([&] {
        const auto repository = state->environment->repository();
        return repository
            && std::ranges::all_of(
                repository->changed_files, [](const imza::ChangedFile& file) {
                    return file.area == imza::ChangedFile::Area::UNSTAGED;
                });
    }));
    CHECK(state->session->error().empty());
}

TEST_CASE("sidebar shows Commit and Push side by side")
{
    if (!imza::test::git_available()) {
        return;
    }
    const imza::test::TempDir scratch;
    seed_staged_and_unstaged(scratch.path);
    auto state = repo_state(scratch.path);
    auto panel = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::BUILD; }, [](imza::WorkflowPhase) { });

    const std::string out
        = imza::test::without_ansi(to_text(panel->Render(), 120, 40));
    const std::size_t commit_at = out.rfind("Commit");
    const std::size_t push_at   = out.rfind("Push");
    REQUIRE(commit_at != std::string::npos);
    REQUIRE(push_at != std::string::npos);
    CHECK(commit_at < push_at);
}

TEST_CASE("clicking a changed file checkbox stages it and clicking the path "
          "opens Review")
{
    if (!imza::test::git_available()) {
        return;
    }
    const imza::test::TempDir scratch;
    seed_staged_and_unstaged(scratch.path);
    auto state    = repo_state(scratch.path);
    int navigated = 0;
    auto panel    = imza::make_side_panel(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 120, 40 }; },
        [] { return imza::WorkflowPhase::BUILD; },
        [&navigated](imza::WorkflowPhase) { ++navigated; });

    // The checkbox sits at the row start; click it without touching the path.
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen, panel->Render());
    const std::vector<std::string> lines = imza::split_lines(screen.ToString());
    int row                              = -1;
    for (std::size_t y = 0; y < lines.size(); ++y) {
        if (imza::test::without_ansi(lines[y]).find("unstaged.txt")
            != std::string::npos) {
            row = static_cast<int>(y);
            break;
        }
    }
    REQUIRE(row >= 0);
    // The glyph's display column, not its byte offset: the border and the
    // checkbox are multi-byte.
    const std::string row_text
        = imza::test::without_ansi(lines[static_cast<std::size_t>(row)]);
    const std::size_t box_byte = row_text.find("☐");
    REQUIRE(box_byte != std::string::npos);
    const std::size_t box_x
        = imza::utf8_width(std::string_view(row_text).substr(0, box_byte));
    ftxui::Mouse click;
    click.button = ftxui::Mouse::Left;
    click.motion = ftxui::Mouse::Pressed;
    click.x      = static_cast<int>(box_x);
    click.y      = row;
    REQUIRE(panel->OnEvent(ftxui::Event::Mouse("", click)));
    CHECK(navigated == 0);
    REQUIRE(imza::test::wait_until([&] {
        const auto repository = state->environment->repository();
        return repository
            && std::ranges::all_of(
                repository->changed_files, [](const imza::ChangedFile& file) {
                    return file.area != imza::ChangedFile::Area::UNSTAGED;
                });
    }));
    CHECK(state->session->error().empty());
    state->session->clear_error();

    REQUIRE(imza::test::click_label(panel, "unstaged.txt", 120, 40));
    CHECK(navigated == 1);
}

TEST_CASE("sidebar renders nothing when no widget has content")
{
    const imza::test::CurrentDirectory directory;
    const imza::test::TempDir scratch;
    auto state = imza::test::make_test_state();
    REQUIRE(state->environment->chdir(scratch.path)
        == imza::Environment::ChdirResult::CHANGED);
    imza::test::wait_for_environment(*state);

    for (const auto kind :
        { imza::LayoutCtx::Kind::NARROW, imza::LayoutCtx::Kind::WIDE }) {
        imza::SidePanelStatus status;
        auto panel = imza::make_side_panel(
            state, [kind] { return imza::LayoutCtx { kind, 120, 40 }; },
            [] { return imza::WorkflowPhase::BUILD; },
            [](imza::WorkflowPhase) { }, &status);

        const ftxui::Element probe
            = ftxui::vbox({ panel->Render(), ftxui::text("MARK") });
        const std::string out
            = imza::test::without_ansi(imza::test::to_text(probe, 120, 10));
        const std::size_t at = out.find("MARK");
        REQUIRE(at != std::string::npos);
        CHECK(out.substr(0, at).find_first_not_of(" \n") == std::string::npos);
        CHECK(status.is_empty());
    }
}

TEST_CASE("skills row hides until a skill is loaded")
{
    imza::RuntimeView view;
    view.skills = { { "caveman", false }, { "anatomy", false } };
    CHECK_FALSE(imza::has_runtime_content(view));
    const std::string unloaded = imza::test::without_ansi(
        to_text(imza::render_runtime_box(view, { }, plain_row_link())));
    CHECK(unloaded.find("Skills") == std::string::npos);
    CHECK(unloaded.find("Runtime") == std::string::npos);

    view.skills[1].loaded = true;
    CHECK(imza::has_runtime_content(view));
    const std::string loaded
        = imza::test::without_ansi(to_text(imza::render_runtime_box(
            view, { imza::RuntimeRow::SKILLS }, plain_row_link())));
    CHECK(loaded.find("Skills") != std::string::npos);
    CHECK(loaded.find("1/2") != std::string::npos);
    CHECK(loaded.find("anatomy") != std::string::npos);
    CHECK(loaded.find("caveman") == std::string::npos);
}

TEST_CASE("runtime permissions markdown reports capabilities and grants")
{
    const imza::RuntimeView view = runtime_view_with_grants();
    const std::string out        = imza::runtime_permissions_markdown(view);

    CHECK(out.find("| Web | off |") != std::string::npos);
    CHECK(out.find("| Shell | on |") != std::string::npos);
    CHECK(out.find("/outside") != std::string::npos);
    CHECK(out.find("cmake --build") != std::string::npos);

    imza::RuntimeView capped          = view;
    capped.permissions.subagent_limit = 3;
    const std::string capped_out = imza::runtime_permissions_markdown(capped);
    CHECK(capped_out.find("| Subagents | max 3 |") != std::string::npos);

    imza::RuntimeView disabled          = view;
    disabled.permissions.subagent_limit = 0;
    CHECK(
        imza::runtime_permissions_markdown(disabled).find("| Subagents | off |")
        != std::string::npos);
    imza::RuntimeView unlimited          = view;
    unlimited.permissions.subagent_limit = imza::MAX_SUBAGENT_TASKS;
    CHECK(imza::runtime_permissions_markdown(unlimited).find(
              "| Subagents | unlimited |")
        != std::string::npos);
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
