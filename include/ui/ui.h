#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/modal.h"
#include "common/tool_call.h"
#include "conversation/session.h"
#include "network/network.h"
#include "permissions/store.h"
#include "tools/skills.h"

namespace imza {

class Session;
class MainThreadQueue;
struct ApplicationState;
struct ReviewHunk;
struct ReviewLine;
struct RepositoryState;

struct LayoutCtx {
    enum class Kind { WIDE, NARROW };
    static constexpr int WIDE_THRESHOLD = 100;
    static constexpr int PANEL_WIDTH    = 40;
    Kind kind                           = Kind::NARROW;
    int width                           = 0;
    int height                          = 0;
};

using LayoutFn = std::function<LayoutCtx()>;

inline const ftxui::Color PANEL_COLOR       = ftxui::Color::RGB(33, 36, 40);
inline const ftxui::Color PANEL_FG          = ftxui::Color::RGB(228, 231, 235);
inline const ftxui::Color PANEL_FG_DIM      = ftxui::Color::RGB(146, 152, 160);
inline const ftxui::Color PANEL_BORDER      = ftxui::Color::RGB(72, 79, 88);
inline const ftxui::Color PANEL_COLOR_FOCUS = ftxui::Color::RGB(45, 50, 56);
inline const ftxui::Color DIFF_ADDITION_BG  = ftxui::Color::RGB(25, 57, 45);
inline const ftxui::Color DIFF_DELETION_BG  = ftxui::Color::RGB(63, 37, 42);
inline const ftxui::Color HL_RED            = ftxui::Color::RGB(247, 114, 114);
inline const ftxui::Color HL_GREEN          = ftxui::Color::RGB(126, 231, 135);
inline const ftxui::Color HL_YELLOW         = ftxui::Color::RGB(242, 204, 96);
inline const ftxui::Color HL_BLUE           = ftxui::Color::RGB(121, 192, 255);
inline const ftxui::Color HL_MAGENTA        = ftxui::Color::RGB(210, 168, 255);
inline const ftxui::Color HL_CYAN           = ftxui::Color::RGB(104, 216, 232);
inline constexpr int MODAL_MAX_WIDTH        = 100;
// Wider frame for the side-by-side diff viewer, which needs two panes of
// readable code; every other modal keeps MODAL_MAX_WIDTH.
inline constexpr int DIFF_VIEWER_MODAL_MAX_WIDTH = 160;

// Width cap for the active modal payload.
int modal_max_width(const ModalPayload& modal);

std::string fit(const std::string& text, int width);
// Byte ranges [begin, end) of the visual rows of one logical line wrapped to
// `width` display columns. Breaks after spaces when one fits, else hard-wraps.
std::vector<std::pair<std::size_t, std::size_t>> wrap_row_ranges(
    std::string_view line, int width);
// Wraps every logical line of `body`; always returns at least one row.
std::vector<std::string> wrap_text(std::string_view body, int width);
ftxui::Element wrapped_input_element(std::string_view content,
    std::size_t cursor, int width, std::string_view placeholder,
    bool focused = true);

ftxui::Element panel(ftxui::Element e);

LayoutCtx layout_context(int width, int height = 0);

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
ftxui::Component split_inline_link_button(std::string primary,
    std::string secondary, std::function<void()> on_click,
    const ftxui::Color& primary_color = HL_GREEN);
std::string elapsed_text(std::chrono::milliseconds elapsed);
std::string compact_number(std::uint64_t n);
ftxui::Element hint_bar(std::string hint);
ftxui::Elements modal_header(std::string title, std::string subtitle = "");

// ◉/○ for single choice, ▣/☐ for multi choice.
std::string choice_marker(bool multi, bool selected);
ftxui::Element choice_label(std::string label, bool selected, bool focused);
bool move_list_cursor(const ftxui::Event& event, int& cursor, int count);

// Captures the unclipped content height of a child element (yframe renders
// report the clipped viewport instead).
ftxui::Decorator capture_content_height(int* out);
ftxui::Decorator capture_content_height(std::function<void(int)> out);

struct ScrollView {
    int scroll         = 0;
    int content_height = 0;
    ftxui::Box box { };

    int viewport_lines() const;
    int max_scroll() const;
    void scroll_lines(int delta);
};

// Renders a viewport over `content` driven by `view`'s scroll offset.
ftxui::Element scroll_viewport(ftxui::Element content, ScrollView& view);
// Shared arrow/page/home/end/wheel handling for a scroll_viewport.
bool scroll_viewport_event(ScrollView& view, ftxui::Event& event);

struct VirtualListWindow {
    std::size_t begin = 0;
    std::size_t end   = 0;
    int before        = 0;
    int after         = 0;
};

class VirtualListState {
public:
    explicit VirtualListState(int estimated_height = 3);

    void reset(std::size_t count);
    void resize(std::size_t count);
    int set_height(std::size_t index, int height);
    int total_height() const;
    VirtualListWindow window(int scroll, int viewport, int overscan) const;

private:
    void _rebuild_offsets() const;

    int _estimated_height = 3;
    std::vector<int> _heights;
    mutable std::vector<std::int64_t> _offsets;
    mutable bool _offsets_dirty = true;
};

struct ModelRow {
    std::string connection_id;
    std::string model_id;
    std::string name;
    std::string tag;
    std::optional<Capabilities> capabilities;
};

ModelRow make_model_row(const std::string& connection_id,
    const std::string& provider_name, const ModelInfo& info);
ftxui::Element model_picker_row(const ModelRow& row, bool selected);

// "image · pdf" labels for advertised input modalities; empty when the
// capabilities are unknown or advertise neither.
std::string capability_tags(const std::optional<Capabilities>& capabilities);

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

// The picker's filter input, wired to refill the visible rows; `pick` must
// outlive the returned component.
ftxui::Component make_model_pick_filter(ModelPickList& pick);
// Arrow keys move the picker selection; false for other events.
bool model_pick_move(ModelPickList& pick, const ftxui::Event& event);
// Appends one row element per visible model entry.
void append_model_pick_rows(const ModelPickList& pick, ftxui::Elements& rows);

// Indices of `rows` whose `match` text contains the lowercased, trimmed
// `filter`. Empty filter selects every row.
std::vector<std::size_t> filter_visible(const std::string& filter,
    std::size_t row_count,
    const std::function<std::string(std::size_t)>& match);

ftxui::Element render_markdown_element(std::string_view md, int width);

bool syntax_type_supported(std::string_view type);
std::string syntax_type_for_path(std::string_view path);
// One vector of visual-row elements per logical line of `code`.
std::vector<std::vector<ftxui::Element>> highlight_code_wrapped(
    std::string_view code, std::string_view type, int width);
// Flattened visual rows of `code`: highlighted when the language is
// supported, else wrapped text in `fallback_fg`.
ftxui::Elements highlighted_rows(std::string_view code, std::string_view syntax,
    int width, ftxui::Color fallback_fg);

// Alt+Enter (both legacy encodings) inserts a newline in multi-line inputs.
inline bool is_alt_enter(const ftxui::Event& event)
{
    return event == ftxui::Event::Special("\x1B\r")
        || event == ftxui::Event::Special("\x1B\n");
}
inline bool is_bracketed_paste_begin(const ftxui::Event& event)
{
    return event == ftxui::Event::Special("\x1B[200~");
}
inline bool is_bracketed_paste_end(const ftxui::Event& event)
{
    return event == ftxui::Event::Special("\x1B[201~");
}
inline bool is_sidechat_toggle(const ftxui::Event& event)
{
    return event == ftxui::Event::CtrlS;
}
// Inserts a newline at `cursor` and advances it.
inline void insert_newline_at(std::string& text, int& cursor)
{
    text.insert(static_cast<std::size_t>(cursor), "\n");
    ++cursor;
}

// Wheel and page scroll steps shared by the scrollers; arrows stay
// component-specific.
inline constexpr int SCROLL_WHEEL_STEP = 3;
inline std::optional<int> scroll_step(ftxui::Event& event, int viewport_lines)
{
    if (event.is_mouse()) {
        if (event.mouse().button == ftxui::Mouse::WheelUp) {
            return -SCROLL_WHEEL_STEP;
        }
        if (event.mouse().button == ftxui::Mouse::WheelDown) {
            return SCROLL_WHEEL_STEP;
        }
        return std::nullopt;
    }
    if (event == ftxui::Event::PageUp) {
        return -std::max(1, viewport_lines - 1);
    }
    if (event == ftxui::Event::PageDown) {
        return std::max(1, viewport_lines - 1);
    }
    return std::nullopt;
}

struct ReviewLineHighlights {
    std::vector<ftxui::Element> old_side;
    std::vector<ftxui::Element> new_side;
};

using ReviewHighlights
    = std::unordered_map<const ReviewLine*, ReviewLineHighlights>;
void append_review_hunk_highlights(ReviewHighlights& cache,
    const ReviewHunk& hunk, std::string_view path, int review_width,
    bool side_by_side);
ftxui::Element review_line_background(ftxui::Element row,
    std::optional<ftxui::Color> change_background, bool selected);

ftxui::Element card(ftxui::Element body,
    std::optional<ftxui::Color> bg = std::nullopt, bool pad = true);
ftxui::Element section_title(
    std::string_view title, ftxui::Color color = PANEL_FG_DIM);
ftxui::Element code_block(
    const std::string& code, const std::string& lang, int width);
ftxui::Element code_block_with_lines(const std::string& code,
    const std::string& lang, std::size_t start_line, int width);
ftxui::Element diff_split(const DiffView& diff, int available_width = 120);
bool diff_row_left_changed(const DiffRow& row);
bool diff_row_right_changed(const DiffRow& row);
// One source for diff glyphs, change backgrounds, and width math.
std::string diff_marker(bool added);
ftxui::Color diff_background(bool added);
int diff_side_width(int width);
int diff_content_width(int width);
// Text width of one diff side after the gutter; the highlight cache and
// the row renderer must agree on it.
inline int review_side_content_width(int side_width)
{
    return std::max(1, side_width - 8);
}
int review_content_width(const LayoutCtx& ctx);
ftxui::Element diffstat_chip(std::size_t additions, std::size_t deletions);
// Draws a canvas module chart as an inline chat element at a fixed
// size that shrinks only when the content is narrower.
ftxui::Element canvas_chart(const CanvasView& view, int available_width);
ftxui::Element session_error_element(const Session& session);

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx);
ftxui::Element render_todo(const TodoList& todo, const LayoutCtx& ctx);
ftxui::Element render_changed_files(
    const RepositoryState& repository, const LayoutCtx& ctx);
ftxui::Element render_context_box(const std::optional<std::string>& rules,
    const std::vector<std::string>& attachments, SkillCounts project_skills,
    SkillCounts global_skills);

struct PermissionView {
    bool web_disabled      = false;
    bool shell_disabled    = false;
    bool approvals_skipped = false;
    std::vector<std::string> folders;
    std::vector<std::string> commands;
};

PermissionView make_permission_view(
    RuntimeFlag flags, const PermissionStore::Grants& grants);
bool has_custom_permissions(const PermissionView& view);
ftxui::Element render_permissions_box(const PermissionView& view);
ftxui::Element render_update_available(std::string version);

// Per-chat footer text; the sidechat clears hints and sets its own input
// hint.
struct ChatHints {
    std::string scroll_line = "Ctrl+↑↓ input history · ↑↓ scroll";
    std::string phase_line
        = "Tab next phase · Shift+Tab previous phase · Ctrl+S Sidechat";
    std::string placeholder = "Ask anything - type / for commands";
    std::string input_hint
        = "  Alt+Enter add line · @ attach file · $ use skill ";
};

ftxui::Component make_chat(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, struct ChatHints hints = { });
ftxui::Component make_side_panel(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow, WorkflowNavigateFn navigate);
ftxui::Component make_review(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowNavigateFn navigate);
ftxui::Component make_status_line(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow);
ftxui::Component make_connect(std::shared_ptr<ApplicationState> state);
ftxui::Component make_subscription_signin(
    std::shared_ptr<ApplicationState> state, std::string connection_id,
    std::function<std::string()> label = { });
ftxui::Component make_subagents(std::shared_ptr<ApplicationState> state);
ftxui::Component make_variant(std::shared_ptr<ApplicationState> state);
ftxui::Component make_sessions(std::shared_ptr<ApplicationState> state);
ftxui::Component make_skills(std::shared_ptr<ApplicationState> state);
ftxui::Component make_modal(std::shared_ptr<ApplicationState> state);
// Sidechat column: owns its chat pane, modal host, focus state, and mouse
// box. `on_focus` fires when the pane gains or loses attention; `status`
// mirrors pane-private UI state for the app screen (UI thread only).
struct SidechatStatus {
    bool focused = false;
    ftxui::Component modal;
    // Live query: pane is open and a modal payload is pending on its session.
    std::function<bool()> has_modal;
};

ftxui::Component make_sidechat_component(
    std::shared_ptr<ApplicationState> state, std::function<void()> on_focus,
    SidechatStatus& status);

int run_repl(
    std::shared_ptr<ApplicationState> state, MainThreadQueue& main_thread);

} // namespace imza
