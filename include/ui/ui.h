#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/diff.h"
#include "network/models.h"
#include "subsystems/skills.h"
#include "subsystems/workflow.h"

namespace ursa {

class ApplicationState;
class Session;
struct RepositoryState;

struct LayoutCtx {
    enum class Kind { WIDE, NARROW };
    static constexpr int wide_threshold = 100;
    static constexpr int panel_width    = 40;
    Kind kind;
    int width;
};

using LayoutFn = std::function<LayoutCtx()>;

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
ftxui::Component inline_link_button(std::function<ftxui::Element()> render,
    std::function<void()> on_click,
    const ftxui::Color& inactive_color = PANEL_FG_DIM);
ftxui::Component inline_link_button(std::string label,
    std::function<void()> on_click,
    const ftxui::Color& inactive_color = PANEL_FG_DIM);
std::string elapsed_text(std::chrono::milliseconds elapsed);
std::string compact_number(std::uint64_t n);
ftxui::Element hint_bar(std::string hint);

// ◉/○ for single choice, ▣/☐ for multi choice.
std::string choice_marker(bool multi, bool selected);
ftxui::Element choice_label(std::string label, bool selected, bool focused);
bool move_list_cursor(const ftxui::Event& event, int& cursor, int count);

// Captures the unclipped content height of a child element (yframe renders
// report the clipped viewport instead).
ftxui::Decorator capture_content_height(int* out);

struct ScrollView {
    int scroll         = 0;
    int content_height = 0;
    ftxui::Box box { };

    int viewport_lines() const;
    int max_scroll() const;
    void scroll_lines(int delta);
};

struct ModelRow {
    std::string connection_id;
    std::string model_id;
    std::string name;
    std::string tag;
};

ModelRow make_model_row(const std::string& connection_id,
    const std::string& provider_name, const ModelInfo& info);
ftxui::Element model_picker_row(const ModelRow& row, bool selected);

// Filterable model list shared by the model pickers.
struct ModelPickList {
    std::vector<ModelRow> rows;
    std::vector<std::size_t> visible;
    int selected = 0;
    std::string filter;
    int filter_cursor = 0;

    void refill_visible();
    void move(int delta);
    const ModelRow* chosen() const;
};

ftxui::Element render_markdown_element(std::string_view md);

bool syntax_type_supported(std::string_view type);
std::string syntax_type_for_path(std::string_view path);
ftxui::Element highlight_code_line(
    std::string_view code, std::string_view type);
ftxui::Elements highlight_code(std::string_view code, std::string_view type);

ftxui::Element card(ftxui::Element body,
    std::optional<ftxui::Color> bg = std::nullopt, bool pad = true);
ftxui::Element section_title(
    std::string_view title, ftxui::Color color = PANEL_FG_DIM);
ftxui::Element code_block(
    const std::string& code, const std::string& lang = "");
ftxui::Element code_block_with_lines(
    const std::string& code, const std::string& lang, std::size_t start_line);
ftxui::Element diff_split(const DiffView& diff, int available_width = 120);
bool diff_row_left_changed(const DiffRow& row);
bool diff_row_right_changed(const DiffRow& row);
ftxui::Element session_error_element(const Session& session);

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx);
ftxui::Element render_todo(const TodoList& todo, const LayoutCtx& ctx);
ftxui::Element render_changed_files(
    const RepositoryState& repository, const LayoutCtx& ctx);
ftxui::Element render_context_box(const std::optional<std::string>& rules,
    const std::vector<std::string>& attachments, SkillCounts project_skills,
    SkillCounts global_skills);

ftxui::Component make_chat(
    std::shared_ptr<ApplicationState> state, LayoutFn layout);
ftxui::Component make_side_panel(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow, WorkflowNavigateFn navigate);
ftxui::Component make_review(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowNavigateFn navigate);
ftxui::Component make_status_line(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow);
ftxui::Component make_connect(std::shared_ptr<ApplicationState> state);
ftxui::Component make_subagents(std::shared_ptr<ApplicationState> state);
ftxui::Component make_variant(std::shared_ptr<ApplicationState> state);
ftxui::Component make_sessions(std::shared_ptr<ApplicationState> state);
ftxui::Component make_skills(std::shared_ptr<ApplicationState> state);
ftxui::Component make_modal(std::shared_ptr<ApplicationState> state);

} // namespace ursa
