#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <string>
#include <string_view>

#include "ui.hpp"

namespace ursa {

inline const ftxui::Color PANEL_COLOR = ftxui::Color::RGB(26, 34, 52);

ftxui::Element render_markdown_element(std::string_view md);

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx);
ftxui::Element render_todo(const TodoList& todo, const LayoutCtx& ctx);
ftxui::Element render_changed_files(
    const std::vector<ChangedFile>& files, const LayoutCtx& ctx);
ftxui::Element render_question(const Question& q);

} // namespace ursa
