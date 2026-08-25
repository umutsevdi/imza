#include <string>

#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ui.h"

namespace {

std::string to_text(ftxui::Element element)
{
    using namespace ftxui;
    auto screen = Screen::Create(Dimension::Fixed(60), Dimension::Fixed(30));
    Render(screen, element);
    return screen.ToString();
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

TEST_CASE("render_changed_files renders status and path")
{
    ursa::ChangedFile f { "src/ui/app.cpp", "M" };
    const std::string out = to_text(
        ursa::render_changed_files({ f }, { ursa::LayoutCtx::Kind::WIDE, 30 }));
    CHECK(out.find("M") != std::string::npos);
    CHECK(out.find("src/ui/app.cpp") != std::string::npos);
}
