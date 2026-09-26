#pragma once

#include <string>

#include "conversation/session.h"
#include "network/network.h"
#include "providers/pricing.h"

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

} // namespace imza::test
