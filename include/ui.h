#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent.h"

namespace ursa {

inline const ftxui::Color PANEL_COLOR  = ftxui::Color::RGB(26, 34, 52);
inline const ftxui::Color PANEL_FG     = ftxui::Color::RGB(228, 232, 240);
inline const ftxui::Color PANEL_FG_DIM = ftxui::Color::RGB(148, 156, 172);
inline const ftxui::Color PANEL_BORDER = ftxui::Color::RGB(78, 89, 110);
inline const ftxui::Color PANEL_COLOR_FOCUS = ftxui::Color::RGB(44, 56, 84);

inline constexpr int MODAL_MAX_WIDTH = 120;

ftxui::Element panel(ftxui::Element e);

ftxui::Element render_markdown_element(std::string_view md);

ftxui::Element card(
    ftxui::Element body, std::optional<ftxui::Color> bg = std::nullopt,
    bool pad = true);
ftxui::Element section_title(
    std::string_view title, ftxui::Color color = PANEL_FG_DIM);
ftxui::Element code_block(const std::string& code, const std::string& lang = "");
ftxui::Element code_block_with_lines(const std::string& code,
    const std::string& lang, std::size_t start_line);
ftxui::Element list_block(const std::string& text);

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx);
ftxui::Element render_todo(const TodoList& todo, const LayoutCtx& ctx);
ftxui::Element render_changed_files(
    const std::vector<ChangedFile>& files, const LayoutCtx& ctx);
ftxui::Element render_help(const std::vector<SlashCommand>& commands);
ftxui::Element status_line(
    const Config& cfg, const UiState& state, const LayoutCtx& ctx);

int run_repl(const Config& cfg);

ftxui::Component make_chat(Controller& controller, std::function<int()> width);
ftxui::Component make_side_panel(
    Controller& controller, std::function<int()> width);
ftxui::Component make_settings(Controller& controller);
ftxui::Component make_modal(Controller& controller);

} // namespace ursa
