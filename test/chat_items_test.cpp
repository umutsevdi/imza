#include <string>

#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "format.h"
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

TEST_CASE("render_item renders a user turn")
{
    ursa::ConversationItem it = ursa::UserTurn { "hello" };
    const std::string out
        = to_text(ursa::render_item(it, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("hello") != std::string::npos);
}

TEST_CASE("render_item renders assistant markdown")
{
    ursa::ConversationItem it = ursa::AssistantTurn { "# Title\n" };
    const std::string out
        = to_text(ursa::render_item(it, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("Title") != std::string::npos);
}

TEST_CASE("render_item renders a modal answer")
{
    ursa::ConversationItem ans = ursa::ModalAnswer { { { { "opt" }, "" } } };
    const std::string out_a
        = to_text(ursa::render_item(ans, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out_a.find("User answered:") != std::string::npos);
    CHECK(out_a.find("opt") != std::string::npos);
}
