#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <string_view>

#include "ui.h"

namespace ursa {

inline const ftxui::Color PANEL_COLOR  = ftxui::Color::RGB(26, 34, 52);
inline const ftxui::Color PANEL_FG     = ftxui::Color::RGB(228, 232, 240);
inline const ftxui::Color PANEL_FG_DIM = ftxui::Color::RGB(148, 156, 172);
inline const ftxui::Color PANEL_BORDER = ftxui::Color::RGB(78, 89, 110);

inline ftxui::Element panel(ftxui::Element e)
{
    return std::move(e) | ftxui::bgcolor(PANEL_COLOR) | ftxui::color(PANEL_FG);
}

ftxui::Element render_markdown_element(std::string_view md);

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx);
ftxui::Element render_todo(const TodoList& todo, const LayoutCtx& ctx);
ftxui::Element render_changed_files(
    const std::vector<ChangedFile>& files, const LayoutCtx& ctx);
ftxui::Element render_question(const Question& q);
ftxui::Element render_help(const std::vector<SlashCommand>& commands);

} // namespace ursa
