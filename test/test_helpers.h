#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/util.h"
#include "conversation/session.h"
#include "network/network.h"
#include "providers/pricing.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace imza::test {

// Records a tool call through the production path (Session::apply).
inline void append_tool(
    imza::Session& session, const imza::ToolCallRequest& req)
{
    session.apply(imza::make_tool_call_event(req), imza::ModelPricing { });
}

inline ftxui::Screen to_screen(
    ftxui::Element element, int width = 60, int height = 30)
{
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen;
}

inline std::string to_text(
    ftxui::Element element, int width = 60, int height = 30)
{
    return to_screen(std::move(element), width, height).ToString();
}

// Visible text of a rendered screen: ANSI-styled cells decode to their
// characters, so byte offsets in the result are click columns.
inline std::string without_ansi(std::string_view input)
{
    std::string out;
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '\x1b' || i + 1 >= input.size()
            || input[i + 1] != '[') {
            out += input[i++];
            continue;
        }
        i += 2;
        while (i < input.size() && (input[i] < '@' || input[i] > '~')) {
            ++i;
        }
        if (i < input.size()) {
            ++i;
        }
    }
    return out;
}

// Render the component and press the left mouse button on the first row
// whose visible text contains `label`; the label's byte offset becomes the
// click column. Returns whether the component accepted the event.
inline bool click_label(ftxui::Component& component, std::string_view label,
    int width = 100, int height = 40)
{
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, component->Render());
    const std::vector<std::string> lines = imza::split_lines(screen.ToString());
    for (std::size_t y = 0; y < lines.size(); ++y) {
        const std::size_t x = without_ansi(lines[y]).find(label);
        if (x == std::string::npos) {
            continue;
        }
        ftxui::Mouse mouse;
        mouse.button = ftxui::Mouse::Left;
        mouse.motion = ftxui::Mouse::Pressed;
        mouse.x      = static_cast<int>(x);
        mouse.y      = static_cast<int>(y);
        if (component->OnEvent(ftxui::Event::Mouse("", mouse))) {
            return true;
        }
    }
    return false;
}

} // namespace imza::test
