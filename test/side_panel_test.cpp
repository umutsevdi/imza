#include <string>

#include <doctest/doctest.h>

#include "environment.h"
#include "test_helpers.h"
#include "ui.h"

using ursa::test::to_screen;
using ursa::test::to_text;

TEST_CASE("render_todo renders as panel when wide, strip when narrow")
{
    using Status = ursa::TodoItem::Status;
    ursa::TodoList todo { { { "a", Status::PENDING },
        { "b", Status::IN_PROGRESS }, { "c", Status::COMPLETED } } };

    const std::string wide = to_text(
        ursa::render_todo(todo, { ursa::LayoutCtx::Kind::WIDE, 120 }));
    CHECK(wide.find("[ ]") != std::string::npos);
    CHECK(wide.find("→") != std::string::npos);
    CHECK(wide.find("[x]") != std::string::npos);

    const std::string narrow = to_text(
        ursa::render_todo(todo, { ursa::LayoutCtx::Kind::NARROW, 60 }));
    CHECK(narrow.find("a") != std::string::npos);
}

TEST_CASE("render_todo wraps long items instead of clipping")
{
    using Status = ursa::TodoItem::Status;
    ursa::TodoList todo {
        { { "investigate the flaky parser regression test", Status::PENDING } }
    };
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(30), ftxui::Dimension::Fixed(10));
    ftxui::Render(screen,
        ursa::render_todo(todo, { ursa::LayoutCtx::Kind::WIDE, 30 })
            | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30));
    const std::string out = screen.ToString();
    CHECK(out.find("investigate") != std::string::npos);
    CHECK(out.find("flaky") != std::string::npos);
    CHECK(out.find("regression") != std::string::npos);
    CHECK(out.find("test") != std::string::npos);
}

TEST_CASE("render_todo wraps to the offered width")
{
    using Status = ursa::TodoItem::Status;
    ursa::TodoList todo {
        { { "rectification certification verification", Status::PENDING } }
    };
    auto render_at = [&todo](int width) {
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(8));
        ftxui::Render(screen,
            ursa::render_todo(todo, { ursa::LayoutCtx::Kind::WIDE, width })
                | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width));
        return screen.ToString();
    };
    CHECK(
        render_at(80).find("rectification certification") != std::string::npos);
    CHECK(
        render_at(30).find("rectification certification") == std::string::npos);
}

TEST_CASE("render_changed_files renders colored symbols and readable paths")
{
    using Kind = ursa::ChangedFile::Kind;
    const ursa::RepositoryState repository {
        .branch = "main",
        .changed_files = {
            { "modified.cpp", Kind::MODIFIED },
            { "added.cpp", Kind::ADDED },
            { "untracked.cpp", Kind::UNTRACKED },
            { "deleted.cpp", Kind::DELETED },
            { "old.cpp -> renamed.cpp", Kind::RENAMED },
            { "source.cpp -> copied.cpp", Kind::COPIED },
            { "conflicted.cpp", Kind::CONFLICTED },
            { "unknown.cpp", Kind::UNKNOWN },
        },
        .changes = { 12, 4, 1 },
    };
    auto screen           = to_screen(ursa::render_changed_files(
        repository, { ursa::LayoutCtx::Kind::WIDE, 30 }));
    const std::string out = screen.ToString();

    CHECK(out.find("●") != std::string::npos);
    CHECK(out.find("modified.cpp") != std::string::npos);
    CHECK(out.find("+") != std::string::npos);
    CHECK(out.find("added.cpp") != std::string::npos);
    CHECK(out.find("?") != std::string::npos);
    CHECK(out.find("untracked.cpp") != std::string::npos);
    CHECK(out.find("−") != std::string::npos);
    CHECK(out.find("deleted.cpp") != std::string::npos);
    CHECK(out.find("→") != std::string::npos);
    CHECK(out.find("old.cpp -> renamed.cpp") != std::string::npos);
    CHECK(out.find("⧉") != std::string::npos);
    CHECK(out.find("source.cpp -> copied.cpp") != std::string::npos);
    CHECK(out.find("!") != std::string::npos);
    CHECK(out.find("conflicted.cpp") != std::string::npos);
    CHECK(out.find("•") != std::string::npos);
    CHECK(out.find("unknown.cpp") != std::string::npos);
    CHECK(out.find("+12") != std::string::npos);
    CHECK(out.find("−4") != std::string::npos);

    CHECK(screen.PixelAt(1, 2).foreground_color == ftxui::Color::YellowLight);
    CHECK(screen.PixelAt(1, 3).foreground_color == ftxui::Color::GreenLight);
    CHECK(screen.PixelAt(1, 4).foreground_color == ftxui::Color::CyanLight);
    CHECK(screen.PixelAt(1, 5).foreground_color == ftxui::Color::RedLight);
    CHECK(screen.PixelAt(3, 2).foreground_color == ursa::PANEL_FG);
}

TEST_CASE("render_context_box lists attachment basenames under files")
{
    const std::string out = to_text(ursa::render_context_box(
        "AGENTS.md", { "main.cpp", "design.md" }, { }, { }));

    CHECK(out.find("Files") != std::string::npos);
    CHECK(out.find("AGENTS.md") != std::string::npos);
    CHECK(out.find("main.cpp") != std::string::npos);
    CHECK(out.find("design.md") != std::string::npos);
}
