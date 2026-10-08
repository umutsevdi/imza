#include "app/flows.h"
#include "common/util.h"
#include "conversation/format.h"
#include "tools/tool_args.h"
#include "turn/delegation.h"
#include "ui/autocomplete.h"
#include "ui/tool_format.h"
#include "ui/ui.h"
#include "workspace/attachments.h"

#include <banner.inc>

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    // Inline cap for a lua run's per-file diffs; beyond this the rest
    // collapses behind a viewer button.
    constexpr std::size_t INLINE_DIFF_ROWS = 25;

    constexpr std::size_t INVALID_VERSION = ~std::size_t { 0 };
    constexpr int DEFAULT_VIEWPORT_LINES  = 24;
    constexpr int TIMELINE_OVERSCAN       = 20;
    constexpr const char* INTERRUPT_HINT  = "Esc interrupt";

    Element empty_state_banner()
    {
        Elements lines;
        std::size_t start = 0;
        while (start <= ui_detail::BANNER.size()) {
            const std::size_t end = ui_detail::BANNER.find('\n', start);
            lines.push_back(
                text(std::string(ui_detail::BANNER.substr(start,
                    end == std::string_view::npos ? std::string_view::npos
                                                  : end - start)))
                | dim);
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return vbox(std::move(lines)) | center;
    }

    std::string assistant_metadata(const AssistantTurn& turn)
    {
        if (turn.model.empty()) {
            return "";
        }
        const std::string effort
            = turn.reasoning_effort.empty() ? "off" : turn.reasoning_effort;
        return turn.model + " · " + effort;
    }

    std::string elapsed_suffix(const Session& session)
    {
        const auto elapsed = session.turn_elapsed();
        if (!elapsed) {
            return "";
        }
        return " " + elapsed_text(*elapsed);
    }

    Element queued_indicator(int frame)
    {
        constexpr int period = PROCESS_PERIOD_FRAMES;
        const auto triangle  = [](int t, int p) {
            t %= 2 * p;
            if (t < 0) {
                t += 2 * p;
            }
            return t < p ? t : 2 * p - t;
        };
        const int travel = triangle(frame, period);
        const int half   = std::max(1, period / 2);
        const int width  = 1
            + std::min(PROCESS_MAX_BLOCKS - 1,
                triangle(travel, half) * PROCESS_MAX_BLOCKS / half);
        const int center = std::min(
            PROCESS_TRACK_BLOCKS - 1, travel * PROCESS_TRACK_BLOCKS / period);
        const int begin
            = std::clamp(center - width / 2, 0, PROCESS_TRACK_BLOCKS - width);
        Elements blocks;
        for (int i = 0; i < PROCESS_TRACK_BLOCKS; ++i) {
            const bool filled = i >= begin && i < begin + width;
            blocks.push_back(
                text("─") | color(filled ? HL_GREEN : PANEL_FG_DIM));
        }
        return hbox(std::move(blocks));
    }

    // Spinner row shown while the agent works: spinner, then a status
    // label or button; the interrupt hint trails everything but the
    // subagent row, which renders alone.
    Element busy_row(int frame, Element middle, bool interrupt_hint = true)
    {
        Elements parts { dim_spinner(frame), std::move(middle) };
        if (interrupt_hint) {
            parts.push_back(filler());
            parts.push_back(text(INTERRUPT_HINT) | dim);
        }
        return hbox(std::move(parts));
    }

    Decorator block_cursor()
    {
        class Impl : public Node {
        public:
            explicit Impl(Element child)
                : Node(Elements { std::move(child) })
            {
            }

            void ComputeRequirement() override
            {
                Node::ComputeRequirement();
                requirement_                      = children_[0]->requirement();
                requirement_.focused.cursor_shape = Screen::Cursor::Block;
            }

            void SetBox(Box box) override
            {
                Node::SetBox(box);
                children_[0]->SetBox(box);
            }
        };
        return [](Element child) {
            return std::make_shared<Impl>(std::move(child));
        };
    }

    // Pending long-running tools whose card animates per frame.
    bool is_running_tool(const ToolCall& tc)
    {
        return (tc.name == "shell" || tc.name == "subagent" || tc.name == "lua")
            && !tc.result.has_value();
    }

    std::size_t item_version(const ConversationItem& it)
    {
        if (const auto* a = std::get_if<AssistantTurn>(&it)) {
            return a->markdown.size() + a->reasoning.size()
                + (a->reasoning_ms ? 1 : 0);
        }
        if (const auto* tc = std::get_if<ToolCall>(&it)) {
            if (!tc->result.has_value()) {
                return tc->phase == ToolCall::Phase::PLANNING ? 0 : 1;
            }
            return 2 + tc->result->text.size();
        }
        if (const auto* event = std::get_if<CompactionEvent>(&it)) {
            return static_cast<std::size_t>(event->status);
        }
        return 0;
    }

    Element user_item(const UserTurn& t, int width)
    {
        Elements rows { render_markdown_element(t.text, width) };
        if (!t.attachments.empty()) {
            Elements chips { filler() };
            for (const auto& attachment : t.attachments) {
                chips.push_back(text(" @" + attachment.path + " ") | inverted);
                chips.push_back(text(" "));
            }
            rows.push_back(hbox(std::move(chips)));
        }
        return card(vbox(std::move(rows)), PANEL_COLOR, false);
    }

    Element assistant_item(std::string_view markdown, int width)
    {
        return card(
            render_markdown_element(markdown, width), std::nullopt, false);
    }

    Element assistant_item(const AssistantTurn& t, int width)
    {
        return assistant_item(std::string_view(t.markdown), width);
    }

    // Interaction maps store either a bare Component or a struct wrapping
    // one (ReasoningLink); this picks the component out of either.
    template <typename T> Component& interaction_component(T& value)
    {
        if constexpr (requires { value.component; }) {
            return value.component;
        } else {
            return value;
        }
    }

    template <typename... Maps>
    bool forward_interaction_event(const Event& event, Maps&... maps)
    {
        const auto forward = [&event](auto& map) {
            for (auto& [key, value] : map) {
                if (interaction_component(value)->OnEvent(event)) {
                    return true;
                }
            }
            return false;
        };
        return (forward(maps) || ...);
    }

    template <typename... Maps>
    void detach_and_clear_interactions(Maps&... maps)
    {
        const auto detach = [](auto& map) {
            for (auto& [key, value] : map) {
                interaction_component(value)->Detach();
            }
            map.clear();
        };
        (detach(maps), ...);
    }

    class ChatImpl : public ComponentBase {
    public:
        ChatImpl(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            ChatHints hints = { })
            : _state(std::move(state))
            , _session(_state->session)
            , _layout(std::move(layout))
            , _hints(std::move(hints))
        {
            _input_options.content         = &_input_buf;
            _input_options.placeholder     = _hints.placeholder.c_str();
            _input_options.multiline       = true;
            _input_options.on_change       = [this] { _on_input_changed(); };
            _input_options.on_enter        = [this] { _submit(); };
            _input_options.cursor_position = Ref<int>(&_input_cursor);
            _input_options.insert          = true;
            _input_options.transform       = [](InputState state) {
                if (state.is_placeholder) {
                    state.element |= dim;
                }
                state.element |= bgcolor(PANEL_COLOR) | block_cursor();
                return state.element;
            };
            _input     = ftxui::Input(_input_options);
            _container = Container::Vertical({ _input });
            Add(_container);
            _input->TakeFocus();
        }

        Element OnRender() override
        {
            const Session& st   = *_session;
            const LayoutCtx ctx = _layout();

            const bool streaming  = st.phase() == Session::Phase::STREAMING;
            const bool connecting = st.phase() == Session::Phase::CONNECTING;
            const bool busy       = streaming || connecting;
            const std::uint64_t content_serial = st.content_serial();
            const std::vector<ConversationItem>& conversation = st.items();
            const std::size_t item_count = conversation.size();
            const std::size_t queued_n   = st.queued().size();
            const bool content_changed   = _content_serial != content_serial;
            const bool layout_changed
                = _cache_kind != ctx.kind || _cache_width != ctx.width;
            const bool reset_cache = content_changed || layout_changed
                || _item_cache.size() > item_count;
            if (reset_cache) {
                _timeline.reset(item_count);
            } else {
                _timeline.resize(item_count);
            }
            _viewport.content_height
                = _timeline.total_height() + static_cast<int>(queued_n);
            if (_follow) {
                _viewport.scroll = _viewport.max_scroll();
            } else {
                _viewport.scroll_lines(0);
            }
            const int _viewport_lines = std::max({ DEFAULT_VIEWPORT_LINES,
                _viewport.viewport_lines(), ctx.height });
            const VirtualListWindow visible
                = _timeline.window(_viewport.scroll, _viewport_lines, 0);
            const VirtualListWindow window = _timeline.window(
                _viewport.scroll, _viewport_lines, TIMELINE_OVERSCAN);
            _anchor_index = visible.begin;
            if (reset_cache) {
                if (item_count == 0) {
                    std::vector<Element>().swap(_item_cache);
                    std::vector<std::size_t>().swap(_item_versions);
                } else {
                    _item_cache.clear();
                    _item_cache.resize(item_count);
                    _item_versions.assign(item_count, INVALID_VERSION);
                }
                _trailing_markdown.reset();
                _trailing_markdown_index = ~std::size_t { 0 };
                _playout_index           = ~std::size_t { 0 };
                _playout_chars           = 0;
                _cache_kind              = ctx.kind;
                _cache_width             = ctx.width;
                _content_serial          = content_serial;
                _cached_begin            = 0;
                _cached_end              = 0;
                _clear_interaction_cache();
            } else if (_item_cache.size() < item_count) {
                const std::size_t previous_size = _item_cache.size();
                _item_cache.resize(item_count);
                _item_versions.resize(item_count, INVALID_VERSION);
                if (previous_size > 0) {
                    _item_versions[previous_size - 1] = INVALID_VERSION;
                }
            }
            _evict_outside(window, conversation);
            if (std::exchange(_hover_dirty, false)) {
                std::fill(_item_versions.begin() + window.begin,
                    _item_versions.begin() + window.end, INVALID_VERSION);
            }
            Elements items;
            if (window.before > 0) {
                items.push_back(vertical_space(window.before));
            }
            for (std::size_t item_index = window.begin; item_index < window.end;
                ++item_index) {
                const ConversationItem& it = conversation[item_index];
                const std::size_t version  = item_version(it);
                const bool is_trailing     = item_index + 1 == item_count;
                const bool active          = is_trailing && streaming
                    && std::holds_alternative<AssistantTurn>(it);
                const bool final_segment = !(is_trailing && busy)
                    && (is_trailing
                        || std::holds_alternative<UserTurn>(
                            conversation[item_index + 1]));
                std::size_t eff_version = version;
                if (const auto* tc = std::get_if<ToolCall>(&it);
                    tc != nullptr && is_running_tool(*tc)) {
                    eff_version = static_cast<std::size_t>(_frame);
                    if (tc->phase == ToolCall::Phase::EXECUTING) {
                        eff_version ^= std::size_t { 1 } << 60;
                    }
                }
                if (final_segment) {
                    eff_version ^= std::size_t { 1 } << 62;
                }
                if (active) {
                    eff_version ^= std::size_t { 1 } << 61;
                }
                // Pacing continues after the turn finishes so the tail
                // drains at display rate instead of popping the remainder;
                // interrupts snap (interrupt_requested stays set until the
                // next turn).
                const bool pacing = is_trailing
                    && std::holds_alternative<AssistantTurn>(it)
                    && (active
                        || (_playout_index == item_index
                            && !st.interrupt_requested()
                            && _playout_chars
                                < std::get<AssistantTurn>(it).markdown.size()));
                if (active) {
                    const auto& at = std::get<AssistantTurn>(it);
                    const bool thinking_now
                        = (!at.reasoning.empty()
                              && !at.reasoning_ms.has_value())
                        || (at.reasoning.empty() && !at.reasoning_ms.has_value()
                            && _reasoning_enabled(at));
                    if (thinking_now) {
                        eff_version = static_cast<std::size_t>(_frame);
                    }
                }
                if (pacing) {
                    // Released characters and parse generations are part of
                    // the rendered identity: the paced element re-renders on
                    // every release and every throttled re-parse.
                    eff_version = (eff_version * 31 + _playout_chars) * 31
                        + _playout_parses;
                }
                Element markdown_element;
                bool markdown_cached = false;
                if (pacing) {
                    const auto& at = std::get<AssistantTurn>(it);
                    const auto now = std::chrono::steady_clock::now();
                    if (_playout_index != item_index) {
                        // A new turn animates from its first character.
                        _playout_index = item_index;
                        _playout_chars = 0;
                    }
                    // Budget rises with the backlog but never past the
                    // display ceiling: without it, the release rate would
                    // converge to the arrival rate and the pacing vanish.
                    const std::size_t backlog
                        = at.markdown.size() - _playout_chars;
                    if (backlog > PLAYOUT_HARD_CAP) {
                        _playout_chars = at.markdown.size();
                        // A snap is a forced repaint: don't let the parse
                        // gate hide it behind the previous element.
                        _trailing_markdown_at = { };
                    } else {
                        const std::size_t budget = std::min(backlog,
                            std::clamp(backlog / 15, std::size_t { 1 },
                                PLAYOUT_MAX_CHARS_PER_TICK));
                        _playout_chars           = std::min(
                            at.markdown.size(), _playout_chars + budget);
                    }
                    const bool due = _trailing_markdown_index != item_index
                        || _item_versions[item_index] == INVALID_VERSION
                        || now - _trailing_markdown_at
                            >= TRAILING_MARKDOWN_INTERVAL;
                    if (due) {
                        _trailing_markdown
                            = assistant_item(std::string_view(at.markdown)
                                                 .substr(0, _playout_chars),
                                _content_width());
                        _trailing_markdown_index = item_index;
                        _trailing_markdown_at    = now;
                        ++_playout_parses;
                    }
                    markdown_element = *_trailing_markdown;
                    markdown_cached  = true;
                }
                if (_item_versions[item_index] != eff_version) {
                    if (std::holds_alternative<ToolCall>(it)) {
                        const ToolCall& tc = std::get<ToolCall>(it);
                        if (!tc.result.has_value()) {
                            _item_cache[item_index] = _render_tool_pending(tc);
                        } else {
                            switch (tc.result->kind) {
                            case ToolCall::Result::Kind::OUTPUT: {
                                if (tc.name == "subagent") {
                                    _item_cache[item_index]
                                        = _render_subagent_item(tc);
                                } else if (tc.name == "lua") {
                                    _item_cache[item_index]
                                        = _render_lua_item(tc);
                                } else {
                                    _item_cache[item_index]
                                        = _render_generic_tool(tc);
                                }
                                break;
                            }
                            case ToolCall::Result::Kind::ERROR:
                                _item_cache[item_index] = _render_tool_flagged(
                                    tc, "Error: ", HL_RED);
                                break;
                            case ToolCall::Result::Kind::REJECT:
                                _item_cache[item_index] = _render_tool_flagged(
                                    tc, "Rejected: ", HL_YELLOW);
                                break;
                            case ToolCall::Result::Kind::CANCEL:
                                _item_cache[item_index]
                                    = _render_tool_pending(tc);
                                break;
                            }
                        }
                    } else if (std::holds_alternative<AssistantTurn>(it)) {
                        _item_cache[item_index]
                            = _render_assistant(std::get<AssistantTurn>(it),
                                item_index, ctx, active, final_segment,
                                markdown_element, markdown_cached);
                    } else {
                        _item_cache[item_index] = render_item(it, ctx);
                    }
                    _item_versions[item_index] = eff_version;
                }
                Element el = _item_cache[item_index];
                if ((streaming || connecting)
                    && std::holds_alternative<AssistantTurn>(it)
                    && is_trailing) {
                    const auto& at = std::get<AssistantTurn>(it);
                    if (at.reasoning.empty()
                        && (!_reasoning_enabled(at) || connecting)) {
                        std::string status
                            = connecting ? " Connecting…" : " Thinking…";
                        status += elapsed_suffix(st);
                        el = vbox({
                            busy_row(_frame,
                                _make_reasoning_button(item_index,
                                    std::move(status), at.reasoning,
                                    assistant_metadata(at))
                                    ->Render()),
                            el,
                        });
                    }
                }
                items.push_back(hbox({ text(" "), std::move(el) | xflex })
                    | capture_content_height(
                        [this, item_index](const int height) {
                            const int delta
                                = _timeline.set_height(item_index, height);
                            if (delta == 0) {
                                return;
                            }
                            if (!_follow && item_index < _anchor_index) {
                                _viewport.scroll
                                    = std::max(0, _viewport.scroll + delta);
                            }
                        }));
            }

            if (window.after > 0) {
                items.push_back(vertical_space(window.after));
            }
            for (size_t i = 0; i < queued_n; ++i) {
                const auto& q = st.queued()[i];
                Elements row {
                    text("[QUEUED] ") | bold | color(HL_GREEN),
                };
                if (i + 1 == queued_n) {
                    row.push_back(text(q.text + "   (ESC to cancel)") | dim);
                } else {
                    row.push_back(text(q.text) | dim);
                }
                items.push_back(hbox(std::move(row)));
            }

            Element content = items.empty()
                ? (_hints.empty_state_banner && st.items().empty()
                          ? empty_state_banner()
                          : text(""))
                : vbox(std::move(items))
                    | capture_content_height(&_viewport.content_height) | flex;
            // Following the tail anchors the bottom of the content: an
            // item whose rendered height outruns the virtual-list
            // estimate (wrapped long lines) keeps its newest lines on
            // screen instead of scrolling them below the fold.
            Element log = std::move(content)
                | (_follow
                        ? focusPositionRelative(0.0f, 1.0f)
                        : focusPosition(0,
                              _viewport.scroll
                                  + std::max(0, _viewport.viewport_lines() - 1)
                                      / 2))
                | vscroll_indicator | yframe;

            Element input_box = panel(vbox({
                separatorEmpty(),
                hbox({
                    text("  "),
                    _input->Render() | xflex,
                    text("  "),
                }),
                separatorEmpty(),
            }));
            Element main      = vbox({
                                    std::move(log) | flex,
                                })
                | flex | reflect(_viewport.box);

            Elements bottom;
            Elements hints;
            if (!_hints.scroll_line.empty()) {
                hints.push_back(hint_bar(_hints.scroll_line));
            }
            const std::string phase_line = _hints.phase_line_fn
                ? _hints.phase_line_fn()
                : _hints.phase_line;
            if (!phase_line.empty()) {
                if (busy) {
                    hints.push_back(hbox({ queued_indicator(_frame), filler(),
                        text(phase_line) | dim }));
                } else {
                    hints.push_back(hint_bar(phase_line));
                }
            }
            if (!hints.empty()) {
                bottom.push_back(vbox(std::move(hints)) | xflex);
            }
            if (_autocomplete.active()) {
                bottom.push_back(_autocomplete.render(ctx));
            }
            Element hint_line = text(_hints.input_hint) | color(PANEL_FG_DIM)
                | bgcolor(PANEL_COLOR);
            bottom.push_back(
                vbox({ std::move(input_box) | yflex, std::move(hint_line) }));
            if (!st.error().empty() || st.retry_countdown()) {
                bottom.push_back(session_error_element(st));
            }
            Elements root;
            root.push_back(std::move(main));
            root.push_back(separatorEmpty());
            for (auto& e : bottom) {
                root.push_back(std::move(e));
            }
            return vbox(std::move(root)) | flex;
        }

        bool OnEvent(Event event) override
        {
            if (is_bracketed_paste_begin(event)) {
                _paste_mode = true;
                return true;
            }
            if (is_bracketed_paste_end(event)) {
                _paste_mode = false;
                return true;
            }
            if (is_alt_enter(event)) {
                _insert_newline();
                return true;
            }
            if (event.is_mouse()) {
                if (event.mouse().motion == Mouse::Moved) {
                    _hover_dirty = true;
                    animation::RequestAnimationFrame();
                }
                if (forward_interaction_event(event, _read_buttons,
                        _subagent_buttons, _diff_buttons, _pending_lua_buttons,
                        _reasoning_links)) {
                    return true;
                }
            }
            if (event == Event::Escape) {
                if (_autocomplete.active()) {
                    _autocomplete.clear();
                    return true;
                }
                if (!_session->queued().empty()) {
                    _session->cancel_queued(_session->queued().back().id);
                    return true;
                }
                if (_session->phase() != Session::Phase::IDLE) {
                    imza::interrupt(*_state);
                    return true;
                }
                return true;
            }
            if (_autocomplete.active()) {
                if (_autocomplete.handle_event(event)) {
                    return true;
                }
                if (event == Event::Return) {
                    if (!_autocomplete.accept(
                            *_state, _input_buf, _input_cursor, _attachments)) {
                        return true;
                    }
                    _submit();
                    return true;
                }
            }
            if (event.is_mouse()) {
                if (event.mouse().button == Mouse::WheelUp
                    || event.mouse().button == Mouse::WheelDown) {
                    _hover_dirty = true;
                    _scroll_lines(*scroll_step(event, _viewport_lines()));
                    return true;
                }
                return false;
            }
            const bool multiline_input
                = _input_buf.find('\n') != std::string::npos;
            if (event == Event::ArrowUpCtrl) {
                _recall_previous_input();
                return true;
            }
            if (event == Event::ArrowDownCtrl) {
                _recall_next_input();
                return true;
            }
            if (!multiline_input && event == Event::ArrowUp) {
                _scroll_lines(-1);
                return true;
            }
            if (!multiline_input && event == Event::ArrowDown) {
                _scroll_lines(1);
                return true;
            }
            if (const std::optional<int> step
                = scroll_step(event, _viewport_lines())) {
                _scroll_lines(*step);
                return true;
            }
            if (event == Event::Return) {
                if (_paste_mode) {
                    _insert_newline();
                    return true;
                }
                if (_input_buf.empty()) {
                    return true;
                }
                _submit();
                return true;
            }
            return _input->OnEvent(event);
        }

        void OnAnimation(animation::Params&) override
        {
            const auto phase = _session->phase();
            if (phase != Session::Phase::IDLE) {
                ++_frame;
                animation::RequestAnimationFrame();
                return;
            }
            // Background compaction outlives the turn: keep the timeline
            // spinner alive while its event is still running.
            if (_session->compaction_running()) {
                ++_frame;
                animation::RequestAnimationFrame();
                return;
            }
            // The playout drain outlives the turn: keep the frame loop
            // alive until the queued tail has fully released, or the
            // response freezes a few characters short.
            if (_playout_index != ~std::size_t { 0 }
                && !_session->interrupt_requested()
                && _playout_index < _session->items().size()) {
                const auto* turn = std::get_if<AssistantTurn>(
                    &_session->items()[_playout_index]);
                if (turn != nullptr && _playout_chars < turn->markdown.size()) {
                    ++_frame;
                    animation::RequestAnimationFrame();
                }
            }
        }

    private:
        int _viewport_lines() const { return _viewport.viewport_lines(); }

        void _scroll_lines(int delta)
        {
            _viewport.scroll_lines(delta);
            _follow = _viewport.scroll == _viewport.max_scroll();
        }

        void _open_viewer_for(const ToolCall& tc)
        {
            if (tc.name == "skill") {
                imza::enqueue_user_modal(*_state,
                    ViewerModal { tool_call_head(tc), tc.result->text,
                        "markdown", 1, true });
            } else if (tc.name == "lua") {
                imza::enqueue_user_modal(*_state,
                    ViewerModal { "Lua execution", lua_viewer_content(tc),
                        "markdown", 1, false });
            } else {
                imza::enqueue_user_modal(*_state,
                    ViewerModal { tool_call_head(tc), tc.result->text, "", 1 });
            }
        }

        void _open_subagent_viewer(const ToolCall& tc, std::size_t index)
        {
            SubagentChat chat = _state->delegation->subagent_chat(tc, index);
            imza::enqueue_user_modal(*_state,
                ViewerModal { std::move(chat.title), std::move(chat.transcript),
                    "markdown", 1, true, "" });
        }

        void _on_input_changed()
        {
            if (!_changing_history) {
                _history_index.reset();
                _history_draft.clear();
            }
            _session->clear_error();
            retain_mentioned_attachments(_input_buf, _attachments);
            _autocomplete.refresh(*_state, _input_buf, _input_cursor);
        }

        void _set_input_from_history(std::string text)
        {
            _changing_history = true;
            _input_buf        = std::move(text);
            _input_cursor     = static_cast<int>(_input_buf.size());
            _on_input_changed();
            _changing_history = false;
        }

        void _recall_previous_input()
        {
            const std::vector<std::string> entries
                = _state->input_history->entries();
            if (entries.empty()) {
                return;
            }
            if (!_history_index) {
                _history_draft = _input_buf;
                _history_index = entries.size();
            }
            if (*_history_index == 0) {
                return;
            }
            --*_history_index;
            _set_input_from_history(entries[*_history_index]);
        }

        void _recall_next_input()
        {
            if (!_history_index) {
                return;
            }
            const std::vector<std::string> entries
                = _state->input_history->entries();
            if (*_history_index + 1 < entries.size()) {
                ++*_history_index;
                _set_input_from_history(entries[*_history_index]);
                return;
            }
            _history_index.reset();
            _set_input_from_history(std::move(_history_draft));
            _history_draft.clear();
        }

        void _insert_newline()
        {
            insert_newline_at(_input_buf, _input_cursor);
            _on_input_changed();
        }

        void _submit()
        {
            const std::string text(_input_buf);
            _input_buf.clear();
            _input_cursor = 0;
            _history_index.reset();
            _history_draft.clear();
            if (!trim(text).empty()) {
                _state->input_history->record(text);
            }
            imza::submit(*_state, text, std::move(_attachments));
            _attachments.clear();
            _autocomplete.clear();
            _follow = true;
            animation::RequestAnimationFrame();
        }

        std::shared_ptr<ApplicationState> _state;
        std::shared_ptr<Session> _session;
        LayoutFn _layout;
        ChatHints _hints;

        Component _container;
        std::map<std::size_t, Component> _read_buttons;
        std::map<std::size_t, Component> _pending_lua_buttons;
        std::map<std::pair<std::size_t, std::size_t>, Component>
            _subagent_buttons;
        std::map<std::pair<std::size_t, std::size_t>, Component> _diff_buttons;
        struct ReasoningLink {
            std::shared_ptr<std::string> label;
            std::shared_ptr<std::string> content;
            std::shared_ptr<std::string> metadata;
            Component component;
        };
        std::map<std::size_t, ReasoningLink> _reasoning_links;

        std::vector<Element> _item_cache;
        std::vector<std::size_t> _item_versions;
        // Throttled markdown element of the streaming trailing item; kept
        // between re-parses so frames stay cheap (see
        // TRAILING_MARKDOWN_INTERVAL).
        std::optional<Element> _trailing_markdown;
        std::size_t _trailing_markdown_index = ~std::size_t { 0 };
        std::chrono::steady_clock::time_point _trailing_markdown_at { };
        // Playout pacing: how much of the active trailing turn's markdown
        // the view has released, advanced per frame by the PLAYOUT_*
        // constants (ui.h). Session truth is never delayed — only pixels.
        std::size_t _playout_index = ~std::size_t { 0 };
        std::size_t _playout_chars = 0;
        // Parse generation of the paced element: bumped on every throttled
        // re-parse so the item version reflects refreshes even after the
        // release has caught up with the truth.
        std::size_t _playout_parses = 0;
        VirtualListState _timeline;
        std::size_t _cached_begin   = 0;
        std::size_t _cached_end     = 0;
        std::size_t _anchor_index   = 0;
        LayoutCtx::Kind _cache_kind = LayoutCtx::Kind::NARROW;
        int _cache_width            = 0;

        void _clear_interaction_cache()
        {
            detach_and_clear_interactions(_read_buttons, _subagent_buttons,
                _diff_buttons, _pending_lua_buttons, _reasoning_links);
        }

        void _evict_item(std::size_t index, const ConversationItem& item)
        {
            if (index < _item_cache.size()) {
                _item_cache[index].reset();
                _item_versions[index] = INVALID_VERSION;
            }
            if (const auto* tool = std::get_if<ToolCall>(&item)) {
                const auto read = _read_buttons.find(tool->id);
                if (read != _read_buttons.end()) {
                    read->second->Detach();
                    _read_buttons.erase(read);
                }
                auto subagent = _subagent_buttons.lower_bound({ tool->id, 0 });
                while (subagent != _subagent_buttons.end()
                    && subagent->first.first == tool->id) {
                    subagent->second->Detach();
                    subagent = _subagent_buttons.erase(subagent);
                }
                auto diff = _diff_buttons.lower_bound({ tool->id, 0 });
                while (diff != _diff_buttons.end()
                    && diff->first.first == tool->id) {
                    diff->second->Detach();
                    diff = _diff_buttons.erase(diff);
                }
                const auto pending = _pending_lua_buttons.find(tool->id);
                if (pending != _pending_lua_buttons.end()) {
                    pending->second->Detach();
                    _pending_lua_buttons.erase(pending);
                }
            }
            const auto reasoning = _reasoning_links.find(index);
            if (reasoning != _reasoning_links.end()) {
                reasoning->second.component->Detach();
                _reasoning_links.erase(reasoning);
            }
            if (_trailing_markdown_index == index) {
                _trailing_markdown.reset();
                _trailing_markdown_index = ~std::size_t { 0 };
            }
            if (_playout_index == index) {
                _playout_index = ~std::size_t { 0 };
                _playout_chars = 0;
                ++_playout_parses;
            }
        }

        void _evict_range(std::size_t begin, std::size_t end,
            const std::vector<ConversationItem>& conversation)
        {
            end = std::min(end, conversation.size());
            for (std::size_t index = begin; index < end; ++index) {
                _evict_item(index, conversation[index]);
            }
        }

        void _evict_outside(const VirtualListWindow& window,
            const std::vector<ConversationItem>& conversation)
        {
            _evict_range(_cached_begin, std::min(_cached_end, window.begin),
                conversation);
            _evict_range(
                std::max(_cached_begin, window.end), _cached_end, conversation);
            _cached_begin = window.begin;
            _cached_end   = window.end;
        }

        Element _tool_header_element(const ToolCall& tc)
        {
            Elements parts {
                text(tc.name == "skill" ? "Load Skill"
                                        : tool_display_name(tc.name))
                    | bold | color(HL_GREEN),
                text(" "),
                text(tool_header_args(tc)) | color(PANEL_FG_DIM),
            };
            if (tc.result.has_value() && tc.result->shell_status.has_value()) {
                const std::string status
                    = shell_status_text(*tc.result->shell_status);
                if (!status.empty()) {
                    parts.push_back(filler());
                    parts.push_back(text(status) | color(HL_RED));
                }
            }
            return hbox(std::move(parts));
        }

        Element _tool_card(const ToolCall& tc, Element body)
        {
            return vbox({
                _tool_header_element(tc),
                std::move(body),
                separatorEmpty(),
            });
        }

        Element _render_viewer_header(const ToolCall& tc, std::size_t count)
        {
            std::string label = tool_header_args(tc);
            label += label.empty() ? "(" : " (";
            label += std::to_string(count);
            label += count == 1 ? " line)" : " lines)";
            Component button = _make_viewer_header_button(tc, std::move(label));
            Elements parts { button->Render() };
            if (tc.result->shell_status.has_value()) {
                const std::string status
                    = shell_status_text(*tc.result->shell_status);
                if (!status.empty()) {
                    parts.push_back(text(status) | color(HL_RED));
                }
            }
            return hbox(std::move(parts));
        }

        const ToolCall* _find_tool_call(std::size_t id) const
        {
            for (const ConversationItem& item : _session->items()) {
                if (const auto* call = std::get_if<ToolCall>(&item);
                    call != nullptr && call->id == id) {
                    return call;
                }
            }
            return nullptr;
        }

        Element _render_tool_status(
            const ToolCall& tc, std::string marker, ftxui::Color tone)
        {
            return _tool_card(tc,
                hbox({
                    text(marker) | bold | color(tone),
                    text(tc.result->text) | color(tone),
                }));
        }

        int _content_width()
        {
            if (_hints.content_width) {
                return std::max(20, _hints.content_width(_layout()) - 4);
            }
            return std::max(20, review_content_width(_layout()) - 4);
        }

        // The default: a collapsed link row that opens the result in the
        // viewer instead of spilling its text into the timeline.
        Element _render_generic_tool(const ToolCall& tc)
        {
            return vbox(
                { _render_viewer_header(tc, count_lines(tc.result->text)),
                    separatorEmpty() });
        }

        Element _render_lua_item(const ToolCall& tc)
        {
            const bool failed = tc.result->kind == ToolCall::Result::Kind::ERROR
                || tc.result->kind == ToolCall::Result::Kind::REJECT;
            const ToolReport report = make_tool_report(tc);
            Component button
                = _make_lua_viewer_button(tc, failed, report.detail);
            Elements rows { button->Render() };
            const LayoutCtx ctx = _layout();
            for (const ToolReportSection& section : report.sections) {
                const auto* report_diff = std::get_if<ToolReportDiff>(&section);
                if (report_diff == nullptr || report_diff->view == nullptr) {
                    const auto* report_canvas
                        = std::get_if<ToolReportCanvas>(&section);
                    if (report_canvas != nullptr
                        && report_canvas->view != nullptr) {
                        rows.push_back(canvas_chart(
                            *report_canvas->view, review_content_width(ctx)));
                    }
                    continue;
                }
                const DiffView& diff  = *report_diff->view;
                std::size_t additions = 0;
                std::size_t deletions = 0;
                for (const DiffRow& row : diff.rows) {
                    deletions += diff_row_left_changed(row) ? 1 : 0;
                    additions += diff_row_right_changed(row) ? 1 : 0;
                }
                rows.push_back(hbox({ text(diff.file) | bold | color(PANEL_FG),
                    filler(), diffstat_chip(additions, deletions) }));
                if (diff.rows.size() <= INLINE_DIFF_ROWS) {
                    rows.push_back(diff_split(diff, review_content_width(ctx)));
                    continue;
                }
                DiffView head;
                head.file = diff.file;
                head.rows.assign(diff.rows.begin(),
                    std::next(diff.rows.begin(),
                        static_cast<std::ptrdiff_t>(INLINE_DIFF_ROWS)));
                rows.push_back(diff_split(head, review_content_width(ctx)));
                const std::string label = "‹ View full diff ("
                    + std::to_string(diff.rows.size()) + " lines) ›";
                rows.push_back(
                    _make_diff_viewer_button(tc.id, report_diff->index, label)
                        ->Render());
            }
            rows.push_back(separatorEmpty());
            return vbox(std::move(rows));
        }

        // Flagged results keep the lua card shape: the status header
        // would echo the whole script into the chat.
        Element _render_tool_flagged(
            const ToolCall& tc, std::string marker, ftxui::Color tone)
        {
            if (tc.name == "lua") {
                return _render_lua_item(tc);
            }
            return _render_tool_status(tc, std::move(marker), tone);
        }

        Element _render_tool_pending(const ToolCall& tc)
        {
            const bool planning = tc.phase == ToolCall::Phase::PLANNING;
            if (planning) {
                // Arguments are still streaming, so there is nothing to
                // inspect yet; show a plain status row.
                return vbox({
                    busy_row(_frame,
                        text(" Planning…" + elapsed_suffix(*_session)) | dim),
                    separatorEmpty(),
                });
            }
            if (tc.name == "subagent") {
                Elements rows {
                    busy_row(_frame,
                        text(" Delegating…" + elapsed_suffix(*_session)) | dim,
                        false),
                };
                for (std::size_t index = 0; index < tc.subagent_ids.size();
                    ++index) {
                    const SubagentChat chat
                        = _state->delegation->subagent_chat(tc, index);
                    rows.push_back(_make_subagent_viewer_button(
                        tc.id, index, "‹ View " + chat.title + " chat ›")
                            ->Render());
                }
                rows.push_back(separatorEmpty());
                return vbox(std::move(rows));
            }
            if (tc.name == "lua") {
                return vbox({
                    busy_row(_frame,
                        hbox({ text(" "),
                            _make_lua_pending_button(tc)->Render() }),
                        false),
                    separatorEmpty(),
                });
            }
            return vbox({
                _tool_header_element(tc),
                separatorEmpty(),
            });
        }

        Element _render_subagent_item(const ToolCall& tc)
        {
            Elements rows { _tool_header_element(tc) };
            const std::size_t count
                = std::max(tc.subagent_ids.size(), tc.subagent_chats.size());
            for (std::size_t index = 0; index < count; ++index) {
                const SubagentChat chat
                    = _state->delegation->subagent_chat(tc, index);
                rows.push_back(_make_subagent_viewer_button(
                    tc.id, index, "‹ View " + chat.title + " chat ›")
                        ->Render());
            }
            rows.push_back(separatorEmpty());
            return vbox(std::move(rows));
        }

        // Find-or-create a component cached per key, attached to the
        // container so its focus state survives across frames.
        template <typename Key, typename Make>
        Component _memoized_button(
            std::map<Key, Component>& cache, Key key, Make&& make)
        {
            if (const auto found = cache.find(key); found != cache.end()) {
                return found->second;
            }
            Component button = make();
            cache.emplace(std::move(key), button);
            _container->Add(button);
            return button;
        }

        template <typename Key>
        Component _memoized_label_button(std::map<Key, Component>& cache,
            Key key, std::string label, std::function<void()> on_click)
        {
            return _memoized_button(cache, std::move(key), [&] {
                auto shared_label
                    = std::make_shared<const std::string>(std::move(label));
                return inline_link_button(
                    [shared_label] { return text(*shared_label); },
                    std::move(on_click), PANEL_FG_DIM);
            });
        }

        Component _make_subagent_viewer_button(
            std::size_t id, std::size_t index, std::string label)
        {
            return _memoized_label_button(_subagent_buttons,
                std::pair { id, index }, std::move(label), [this, id, index] {
                    if (const auto* call = _find_tool_call(id);
                        call != nullptr) {
                        _open_subagent_viewer(*call, index);
                    }
                });
        }

        Component _make_diff_viewer_button(
            std::size_t id, std::size_t index, std::string label)
        {
            return _memoized_label_button(_diff_buttons,
                std::pair { id, index }, std::move(label), [this, id, index] {
                    const auto* call = _find_tool_call(id);
                    if (call == nullptr || !call->result.has_value()
                        || index >= call->result->diffs.size()) {
                        return;
                    }
                    const DiffView& diff = call->result->diffs[index];
                    imza::enqueue_user_modal(*_state,
                        ViewerModal {
                            diff.file, "", "diff", 1, false, "", diff });
                });
        }

        Component _make_lua_pending_button(const ToolCall& tc)
        {
            return _memoized_label_button(
                _pending_lua_buttons, tc.id, "Executing…", [this, id = tc.id] {
                    const auto* call = _find_tool_call(id);
                    if (call == nullptr) {
                        return;
                    }
                    if (call->result.has_value()) {
                        _open_viewer_for(*call);
                        return;
                    }
                    LuaToolArgs parsed;
                    (void)json_parse_checked(call->args, parsed);
                    imza::enqueue_user_modal(*_state,
                        ViewerModal { "Lua script", parsed.script, "lua", 1 });
                });
        }

        Component _make_lua_viewer_button(
            const ToolCall& tc, bool failed, const std::string& counts)
        {
            return _memoized_button(_read_buttons, tc.id, [&] {
                std::string label = failed ? "Execution Failed" : "Executed";
                if (!counts.empty()) {
                    label += " · " + counts;
                }
                auto shared_label
                    = std::make_shared<const std::string>(std::move(label));
                const std::size_t id = tc.id;
                return inline_link_button(
                    [failed, shared_label] {
                        return failed ? text(*shared_label) | strikethrough
                                      : text(*shared_label) | bold;
                    },
                    [this, id] {
                        if (const auto* call = _find_tool_call(id);
                            call != nullptr) {
                            _open_viewer_for(*call);
                        }
                    },
                    failed ? PANEL_FG_DIM : HL_GREEN);
            });
        }

        Component _make_viewer_header_button(
            const ToolCall& tc, std::string detail)
        {
            return _memoized_button(_read_buttons, tc.id, [&] {
                std::string name     = tc.name == "skill"
                    ? "Load Skill"
                    : tool_display_name(tc.name);
                const std::size_t id = tc.id;
                return split_inline_link_button(
                    std::move(name), std::move(detail), [this, id] {
                        if (const auto* call = _find_tool_call(id);
                            call != nullptr) {
                            _open_viewer_for(*call);
                        }
                    });
            });
        }

        bool _reasoning_enabled(const AssistantTurn& turn) const
        {
            return !turn.reasoning_effort.empty()
                && turn.reasoning_effort != "off";
        }

        Element _render_assistant(const AssistantTurn& t, std::size_t index,
            const LayoutCtx&, bool active, bool show_metadata,
            Element markdown_element, bool markdown_cached)
        {
            Elements parts;
            const bool has_reasoning = !trim(t.reasoning).empty();
            const bool expected      = _reasoning_enabled(t);
            const bool done          = t.reasoning_ms.has_value();
            const bool placeholder
                = active && !has_reasoning && !done && expected;
            if (has_reasoning || placeholder) {
                std::string label;
                if (done) {
                    const double secs
                        = static_cast<double>(t.reasoning_ms->count()) / 1000.0;
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f", secs);
                    label = "▸ Thought " + std::string(buf) + "s";
                } else {
                    label = " Thinking…" + elapsed_suffix(*_session);
                }
                Component btn = _make_reasoning_button(index, label,
                    placeholder ? std::string() : t.reasoning,
                    assistant_metadata(t));
                Element row
                    = done ? btn->Render() : busy_row(_frame, btn->Render());
                parts.push_back(row);
            }
            if (!t.markdown.empty()) {
                parts.push_back(markdown_cached
                        ? markdown_element
                        : assistant_item(t, _content_width()));
            }
            if (show_metadata) {
                parts.push_back(hint_bar(assistant_metadata(t)));
            }
            return vbox(std::move(parts));
        }

        Component _make_reasoning_button(std::size_t index, std::string label,
            const std::string& content, std::string metadata)
        {
            if (auto it = _reasoning_links.find(index);
                it != _reasoning_links.end()) {
                *it->second.label    = std::move(label);
                *it->second.content  = content;
                *it->second.metadata = std::move(metadata);
                return it->second.component;
            }
            ReasoningLink entry;
            entry.label    = std::make_shared<std::string>(std::move(label));
            entry.content  = std::make_shared<std::string>(content);
            entry.metadata = std::make_shared<std::string>(std::move(metadata));
            auto label_ptr = entry.label;
            auto content_ptr  = entry.content;
            auto metadata_ptr = entry.metadata;
            entry.component
                = inline_link_button([label_ptr] { return text(*label_ptr); },
                    [this, content_ptr, metadata_ptr] {
                        ViewerModal vm { " Thinking", *content_ptr, "md", 1 };
                        vm.line_numbers = false;
                        vm.metadata     = *metadata_ptr;
                        enqueue_user_modal(*_state, vm);
                    },
                    PANEL_FG_DIM);
            _container->Add(entry.component);
            _reasoning_links.emplace(index, std::move(entry));
            return _reasoning_links.find(index)->second.component;
        }

        std::string _input_buf;
        InputOption _input_options;
        Component _input;

        Autocomplete _autocomplete;
        std::vector<Attachment> _attachments;
        std::optional<std::size_t> _history_index;
        std::string _history_draft;
        int _input_cursor      = 0;
        bool _changing_history = false;
        bool _paste_mode       = false;

        bool _follow                  = true;
        bool _hover_dirty             = false;
        int _frame                    = 0;
        std::uint64_t _content_serial = 0;
        ScrollView _viewport { };
    };

} // namespace

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx)
{
    return std::visit(
        [&](const auto& v) -> Element {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, UserTurn>) {
                return user_item(
                    v, std::max(20, review_content_width(ctx) - 4));
            } else if constexpr (std::is_same_v<T, TodoList>) {
                return render_todo(v, ctx);
            } else if constexpr (std::is_same_v<T, CompactionEvent>) {
                if (v.status == CompactionEvent::Status::COMPLETED) {
                    return text("✓ Session compacted") | dim;
                }
                if (v.status == CompactionEvent::Status::FAILED) {
                    return text("Compaction failed") | dim;
                }
                return text("Compacting…") | dim;
            }
            return text("");
        },
        item);
}

ftxui::Component make_chat(
    std::shared_ptr<ApplicationState> state, LayoutFn layout, ChatHints hints)
{
    return ftxui::Make<ChatImpl>(
        std::move(state), std::move(layout), std::move(hints));
}

} // namespace imza
