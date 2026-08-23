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
    ursa::TodoList todo { { { "a", false }, { "b", true } } };

    const std::string wide = to_text(
        ursa::render_todo(todo, { ursa::LayoutCtx::Kind::WIDE, 120 }));
    CHECK(wide.find("[ ]") != std::string::npos);
    CHECK(wide.find("[x]") != std::string::npos);

    const std::string narrow = to_text(
        ursa::render_todo(todo, { ursa::LayoutCtx::Kind::NARROW, 60 }));
    CHECK(narrow.find("a") != std::string::npos);
}

TEST_CASE("render_changed_files renders status and path")
{
    ursa::ChangedFile f { "src/ui/app.cpp", "M" };
    const std::string out = to_text(
        ursa::render_changed_files({ f }, { ursa::LayoutCtx::Kind::WIDE, 30 }));
    CHECK(out.find("M") != std::string::npos);
    CHECK(out.find("src/ui/app.cpp") != std::string::npos);
}
