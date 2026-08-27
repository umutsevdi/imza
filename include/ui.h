#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "commands.h"
#include "controller.h"

namespace ursa {

using LayoutFn = std::function<LayoutCtx()>;

inline const ftxui::Color PANEL_COLOR       = ftxui::Color::RGB(26, 34, 52);
inline const ftxui::Color PANEL_FG          = ftxui::Color::RGB(228, 232, 240);
inline const ftxui::Color PANEL_FG_DIM      = ftxui::Color::RGB(148, 156, 172);
inline const ftxui::Color PANEL_BORDER      = ftxui::Color::RGB(78, 89, 110);
inline const ftxui::Color PANEL_COLOR_FOCUS = ftxui::Color::RGB(44, 56, 84);

inline constexpr int MODAL_MAX_WIDTH = 100;

ftxui::Element panel(ftxui::Element e);

LayoutCtx layout_context(int width);

ftxui::Component space_activates(
    ftxui::Component child, std::function<void()> on_space);

ftxui::InputOption field_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change = { },
    std::function<void()> on_enter = { });
ftxui::InputOption password_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change = { });
ftxui::Component action_button(std::string label,
    std::function<void()> on_click, const ftxui::Color& color = PANEL_BORDER,
    const ftxui::Color& color_focussed = PANEL_COLOR);

ftxui::Element render_markdown_element(std::string_view md);

ftxui::Element card(ftxui::Element body,
    std::optional<ftxui::Color> bg = std::nullopt, bool pad = true);
ftxui::Element section_title(
    std::string_view title, ftxui::Color color = PANEL_FG_DIM);
ftxui::Element code_block(
    const std::string& code, const std::string& lang = "");
ftxui::Element code_block_with_lines(
    const std::string& code, const std::string& lang, std::size_t start_line);
ftxui::Element diff_split(const DiffView& diff);

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx);
ftxui::Element render_todo(const TodoList& todo, const LayoutCtx& ctx);
ftxui::Element render_changed_files(
    const std::vector<ChangedFile>& files, const LayoutCtx& ctx);
ftxui::Element render_help(const std::vector<SlashCommand>& commands);

int run_repl(const Config& cfg);

ftxui::Component make_chat(std::shared_ptr<Session> session,
    Controller& controller, LayoutFn layout);
ftxui::Component make_side_panel(std::shared_ptr<Session> session,
    Controller& controller, LayoutFn layout);
ftxui::Component make_status_line(std::shared_ptr<Session> session,
    Controller& controller, LayoutFn layout);
ftxui::Component make_connect(
    std::shared_ptr<Session> session, Controller& controller);
ftxui::Component make_variant(
    std::shared_ptr<Session> session, Controller& controller);
ftxui::Component make_modal(
    std::shared_ptr<Session> session, Controller& controller);

} // namespace ursa
