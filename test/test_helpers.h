#pragma once

#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace ursa::test {

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

} // namespace ursa::test
