#include <string>

#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "environment.h"
#include "ui.h"

namespace {

ftxui::Screen to_screen(ftxui::Element element)
{
    using namespace ftxui;
    auto screen = Screen::Create(Dimension::Fixed(60), Dimension::Fixed(30));
    Render(screen, element);
    return screen;
}

std::string to_text(ftxui::Element element)
{
    return to_screen(std::move(element)).ToString();
}

} // namespace

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
    ursa::TodoList todo { { { "investigate the flaky parser regression test",
        Status::PENDING } } };
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(30), ftxui::Dimension::Fixed(10));
    ftxui::Render(screen,
        ursa::render_todo(
            todo, { ursa::LayoutCtx::Kind::WIDE, 30 })
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
    ursa::TodoList todo { { { "rectification certification verification",
        Status::PENDING } } };
    auto render_at = [&todo](int width) {
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(8));
        ftxui::Render(screen,
            ursa::render_todo(todo, { ursa::LayoutCtx::Kind::WIDE, width })
                | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width));
        return screen.ToString();
    };
    CHECK(render_at(80).find("rectification certification")
        != std::string::npos);
    CHECK(render_at(30).find("rectification certification")
        == std::string::npos);
}

TEST_CASE("render_changed_files renders colored symbols and readable paths")
{
    using Kind = ursa::ChangedFile::Kind;
    const std::vector<ursa::ChangedFile> files {
        { "modified.cpp", Kind::MODIFIED },
        { "added.cpp", Kind::ADDED },
        { "untracked.cpp", Kind::UNTRACKED },
        { "deleted.cpp", Kind::DELETED },
        { "old.cpp -> renamed.cpp", Kind::RENAMED },
        { "source.cpp -> copied.cpp", Kind::COPIED },
        { "conflicted.cpp", Kind::CONFLICTED },
        { "unknown.cpp", Kind::UNKNOWN },
    };
    auto screen = to_screen(ursa::render_changed_files(
        files, { ursa::LayoutCtx::Kind::WIDE, 30 }));
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

    CHECK(screen.PixelAt(1, 2).foreground_color == ftxui::Color::YellowLight);
    CHECK(screen.PixelAt(1, 3).foreground_color == ftxui::Color::GreenLight);
    CHECK(screen.PixelAt(1, 4).foreground_color == ftxui::Color::CyanLight);
    CHECK(screen.PixelAt(1, 5).foreground_color == ftxui::Color::RedLight);
    CHECK(screen.PixelAt(3, 2).foreground_color == ursa::PANEL_FG);
}
