#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <functional>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "controller.h"

namespace ursa {

struct RepositoryState;

using LayoutFn = std::function<LayoutCtx()>;

enum class WorkflowPhase { PLAN, BUILD, REVIEW };

WorkflowPhase next_workflow_phase(
    WorkflowPhase phase, bool review_available);
WorkflowPhase previous_workflow_phase(
    WorkflowPhase phase, bool review_available);
std::optional<Session::Mode> workflow_mode(WorkflowPhase phase);

using WorkflowFn = std::function<WorkflowPhase()>;
using WorkflowNavigateFn = std::function<void(WorkflowPhase)>;

inline const ftxui::Color PANEL_COLOR       = ftxui::Color::RGB(26, 34, 52);
inline const ftxui::Color PANEL_FG          = ftxui::Color::RGB(228, 232, 240);
inline const ftxui::Color PANEL_FG_DIM      = ftxui::Color::RGB(148, 156, 172);
inline const ftxui::Color PANEL_BORDER      = ftxui::Color::RGB(78, 89, 110);
inline const ftxui::Color PANEL_COLOR_FOCUS = ftxui::Color::RGB(44, 56, 84);
inline const ftxui::Color DIFF_ADDITION_BG  = ftxui::Color::RGB(24, 67, 50);
inline const ftxui::Color DIFF_DELETION_BG  = ftxui::Color::RGB(78, 39, 46);

inline constexpr int MODAL_MAX_WIDTH = 100;

std::string fit(const std::string& text, int width);
std::string fit(const std::string& text, int width, int offset);

ftxui::Element panel(ftxui::Element e);

LayoutCtx layout_context(int width);

ftxui::Component space_activates(
    ftxui::Component child, std::function<void()> on_space);

ftxui::InputOption field_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change = { },
    std::function<void()> on_enter = { });
ftxui::InputOption multiline_field_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change = { });
ftxui::InputOption password_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change = { });
ftxui::Component action_button(std::string label,
    std::function<void()> on_click, const ftxui::Color& color = PANEL_BORDER,
    const ftxui::Color& color_focussed = PANEL_COLOR_FOCUS);
ftxui::Component inline_link_button(
    std::function<ftxui::Element()> render, std::function<void()> on_click,
    const ftxui::Color& inactive_color = PANEL_FG_DIM);
ftxui::Component inline_link_button(
    std::string label, std::function<void()> on_click,
    const ftxui::Color& inactive_color = PANEL_FG_DIM);
std::string elapsed_text(std::chrono::milliseconds elapsed);

ftxui::Element render_markdown_element(std::string_view md);

ftxui::Element card(ftxui::Element body,
    std::optional<ftxui::Color> bg = std::nullopt, bool pad = true);
ftxui::Element section_title(
    std::string_view title, ftxui::Color color = PANEL_FG_DIM);
ftxui::Element code_block(
    const std::string& code, const std::string& lang = "");
ftxui::Element code_block_with_lines(
    const std::string& code, const std::string& lang, std::size_t start_line);
ftxui::Element diff_split(const DiffView& diff, int available_width = 120);
ftxui::Element session_error_element(const Session& session);

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx);
ftxui::Element render_todo(const TodoList& todo, const LayoutCtx& ctx);
ftxui::Element render_changed_files(
    const RepositoryState& repository, const LayoutCtx& ctx);
ftxui::Element render_context_box(const std::optional<std::string>& rules,
    const std::vector<std::string>& attachments, SkillCounts project_skills,
    SkillCounts global_skills);
ftxui::Element render_context_box(const std::optional<std::string>& rules,
    const std::vector<std::string>& attachments, int project_skills,
    int global_skills);
int run_repl(const Config& cfg);
void print_session_saved_box();

ftxui::Component make_chat(
    std::shared_ptr<ApplicationState> state, Controller& controller,
    LayoutFn layout);
ftxui::Component make_side_panel(
    std::shared_ptr<ApplicationState> state, Controller& controller,
    LayoutFn layout, WorkflowFn workflow, WorkflowNavigateFn navigate);
ftxui::Component make_review(
    std::shared_ptr<ApplicationState> state, Controller& controller,
    LayoutFn layout, WorkflowNavigateFn navigate);
ftxui::Component make_status_line(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow);
ftxui::Component make_connect(
    std::shared_ptr<ApplicationState> state, Controller& controller);
ftxui::Component make_variant(
    std::shared_ptr<ApplicationState> state, Controller& controller);
ftxui::Component make_sessions(
    std::shared_ptr<ApplicationState> state, Controller& controller);
ftxui::Component make_skills(
    std::shared_ptr<ApplicationState> state, Controller& controller);
ftxui::Component make_modal(
    std::shared_ptr<ApplicationState> state, Controller& controller);

} // namespace ursa
