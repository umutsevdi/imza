#include "app/flows.h"
#include "common/util.h"
#include "conversation/format.h"
#include "turn/delegation.h"
#include "ui/autocomplete.h"
#include "ui/tool_format.h"
#include "ui/ui.h"
#include "workspace/attachments.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    using namespace ftxui;

    constexpr std::size_t kLargeOutputLines = 10;
    constexpr std::size_t kInvalidVersion   = ~std::size_t { 0 };
    constexpr int kWheelStep                = 3;
    constexpr int kDefaultViewportLines     = 24;
    constexpr int kTimelineOverscan         = 20;

    Element vertical_space(int height)
    {
        return text("") | size(HEIGHT, EQUAL, std::max(0, height));
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
                requirement_ = children_[0]->requirement();
                requirement_.focused.cursor_shape
                    = Screen::Cursor::BlockBlinking;
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

    std::size_t item_version(const ConversationItem& it)
    {
        if (const auto* a = std::get_if<AssistantTurn>(&it)) {
            return a->markdown.size() + a->reasoning.size()
                + (a->reasoning_ms ? 1 : 0);
        }
        if (const auto* tc = std::get_if<ToolCall>(&it)) {
            if (!tc->result.has_value()) {
                return 0;
            }
            return 1 + tc->result->text.size();
        }
        if (const auto* event = std::get_if<CompactionEvent>(&it)) {
            return static_cast<std::size_t>(event->status);
        }
        return 0;
    }

    Element user_item(const UserTurn& t)
    {
        Elements rows { render_markdown_element(t.text) };
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

    Element assistant_item(const AssistantTurn& t)
    {
        return card(render_markdown_element(t.markdown), std::nullopt, false);
    }

    Element modal_answer_item(const ModalAnswer& ans)
    {
        return card(render_markdown_element(modal_answer_markdown(ans)),
            PANEL_COLOR, false);
    }

    class ChatImpl : public ComponentBase {
    public:
        ChatImpl(std::shared_ptr<ApplicationState> state, LayoutFn layout)
            : state_(std::move(state))
            , session_(state_->session)
            , layout_(std::move(layout))
        {
            input_options_.content     = &input_buf_;
            input_options_.placeholder = "Ask anything — type / for commands";
            input_options_.multiline   = true;
            input_options_.on_change   = [this] { on_input_changed(); };
            input_options_.on_enter    = [this] { submit(); };
            input_options_.cursor_position = Ref<int>(&input_cursor_);
            input_options_.insert          = true;
            input_options_.transform       = [](InputState state) {
                if (state.is_placeholder) {
                    state.element |= dim;
                }
                state.element |= bgcolor(PANEL_COLOR) | block_cursor();
                return state.element;
            };
            input_     = ftxui::Input(input_options_);
            container_ = Container::Vertical({ input_ });
            Add(container_);
            input_->TakeFocus();
        }

        Element OnRender() override
        {
            const Session& st   = *session_;
            const LayoutCtx ctx = layout_();

            const bool streaming    = st.phase() == Session::Phase::STREAMING;
            const bool connecting   = st.phase() == Session::Phase::CONNECTING;
            const bool busy         = streaming || connecting;
            const bool tool_running = busy
                && std::any_of(st.items().begin(), st.items().end(),
                    [](const ConversationItem& item) {
                        const auto* tc = std::get_if<ToolCall>(&item);
                        return tc != nullptr
                            && (tc->name == "shell" || tc->name == "subagent")
                            && !tc->result.has_value();
                    });
            const bool compaction_running = std::any_of(st.items().begin(),
                st.items().end(), [](const ConversationItem& item) {
                    const auto* event = std::get_if<CompactionEvent>(&item);
                    return event != nullptr
                        && event->status == CompactionEvent::Status::RUNNING;
                });
            if (tool_running || compaction_running) {
                animation::RequestAnimationFrame();
            }

            const std::uint64_t content_serial = st.content_serial();
            const std::vector<ConversationItem>& conversation = st.items();
            const std::size_t item_count = conversation.size();
            const std::size_t queued_n   = st.queued().size();
            const bool content_changed   = content_serial_ != content_serial;
            const bool layout_changed
                = cache_kind_ != ctx.kind || cache_width_ != ctx.width;
            if (content_changed || layout_changed
                || item_cache_.size() > item_count) {
                _timeline.reset(item_count);
            } else {
                _timeline.resize(item_count);
            }
            viewport_.content_height
                = _timeline.total_height() + static_cast<int>(queued_n);
            if (follow_) {
                viewport_.scroll = viewport_.max_scroll();
            } else {
                viewport_.scroll_lines(0);
            }
            const int viewport_lines = std::max({ kDefaultViewportLines,
                viewport_.viewport_lines(), ctx.height });
            const VirtualListWindow visible
                = _timeline.window(viewport_.scroll, viewport_lines, 0);
            const VirtualListWindow window = _timeline.window(
                viewport_.scroll, viewport_lines, kTimelineOverscan);
            _anchor_index          = visible.begin;
            const bool reset_cache = layout_changed || content_changed
                || item_cache_.size() > item_count;
            if (reset_cache) {
                item_cache_.clear();
                item_cache_.resize(item_count);
                item_versions_.assign(item_count, kInvalidVersion);
                cache_kind_     = ctx.kind;
                cache_width_    = ctx.width;
                content_serial_ = content_serial;
                _cached_begin   = 0;
                _cached_end     = 0;
                clear_interaction_cache();
            } else if (item_cache_.size() < item_count) {
                const std::size_t previous_size = item_cache_.size();
                item_cache_.resize(item_count);
                item_versions_.resize(item_count, kInvalidVersion);
                if (previous_size > 0) {
                    item_versions_[previous_size - 1] = kInvalidVersion;
                }
            }
            evict_outside(window, conversation);
            if (std::exchange(hover_dirty_, false)) {
                std::fill(item_versions_.begin() + window.begin,
                    item_versions_.begin() + window.end, kInvalidVersion);
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
                if (const auto* tc = std::get_if<ToolCall>(&it); tc != nullptr
                    && (tc->name == "shell" || tc->name == "subagent")
                    && !tc->result.has_value()) {
                    eff_version = static_cast<std::size_t>(frame_);
                }
                if (final_segment) {
                    eff_version ^= std::size_t { 1 } << 62;
                }
                if (active) {
                    eff_version ^= std::size_t { 1 } << 61;
                }
                if (active) {
                    const auto& at = std::get<AssistantTurn>(it);
                    const bool thinking_now
                        = (!at.reasoning.empty()
                              && !at.reasoning_ms.has_value())
                        || (at.reasoning.empty() && !at.reasoning_ms.has_value()
                            && reasoning_enabled(at));
                    if (thinking_now) {
                        eff_version = static_cast<std::size_t>(frame_);
                    }
                }
                if (item_versions_[item_index] != eff_version) {
                    if (std::holds_alternative<ToolCall>(it)) {
                        const ToolCall& tc = std::get<ToolCall>(it);
                        if (!tc.result.has_value()) {
                            item_cache_[item_index] = render_tool_pending(tc);
                        } else {
                            switch (tc.result->kind) {
                            case ToolCall::Result::Kind::OUTPUT: {
                                const bool big = count_lines(tc.result->text)
                                    > kLargeOutputLines;
                                if (tc.name == "subagent") {
                                    item_cache_[item_index]
                                        = render_subagent_item(tc);
                                } else if (tc.name == "read") {
                                    item_cache_[item_index]
                                        = render_read_item(tc);
                                } else if (tc.name == "skill") {
                                    item_cache_[item_index]
                                        = render_skill_item(tc);
                                } else if (tc.name == "list") {
                                    item_cache_[item_index]
                                        = render_list_collapsed(tc);
                                } else if (tc.name == "shell") {
                                    item_cache_[item_index] = big
                                        ? render_shell_collapsed(tc)
                                        : render_shell_item(tc);
                                } else if (tc.name == "edit"
                                    || tc.name == "write") {
                                    item_cache_[item_index]
                                        = render_write_item(tc);
                                } else if (tc.name == "webfetch"
                                    || tc.name == "websearch") {
                                    item_cache_[item_index]
                                        = render_web_item(tc);
                                } else if (tc.name == "ask") {
                                    item_cache_[item_index]
                                        = render_ask_item(tc);
                                } else {
                                    item_cache_[item_index]
                                        = render_generic_tool(tc);
                                }
                                break;
                            }
                            case ToolCall::Result::Kind::ERROR:
                                item_cache_[item_index] = render_tool_error(tc);
                                break;
                            case ToolCall::Result::Kind::REJECT:
                                item_cache_[item_index]
                                    = render_tool_reject(tc);
                                break;
                            case ToolCall::Result::Kind::CANCEL:
                                item_cache_[item_index]
                                    = render_tool_pending(tc);
                                break;
                            }
                        }
                    } else if (std::holds_alternative<AssistantTurn>(it)) {
                        item_cache_[item_index]
                            = render_assistant(std::get<AssistantTurn>(it),
                                item_index, ctx, active, final_segment);
                    } else {
                        item_cache_[item_index] = render_item(it, ctx);
                    }
                    item_versions_[item_index] = eff_version;
                }
                Element el = item_cache_[item_index];
                if (const auto* event = std::get_if<CompactionEvent>(&it);
                    event != nullptr
                    && event->status == CompactionEvent::Status::RUNNING) {
                    el = hbox({
                        spinner(15, static_cast<size_t>(frame_))
                            | color(PANEL_FG_DIM),
                        text(" Compacting…") | dim,
                    });
                }
                if ((streaming || connecting)
                    && std::holds_alternative<AssistantTurn>(it)
                    && is_trailing) {
                    const auto& at = std::get<AssistantTurn>(it);
                    if (at.reasoning.empty()
                        && (!reasoning_enabled(at) || connecting)) {
                        std::string status
                            = connecting ? " Connecting…" : " Thinking…";
                        status += elapsed_suffix(st);
                        el = vbox({
                            hbox({
                                spinner(15, static_cast<size_t>(frame_))
                                    | color(PANEL_FG_DIM),
                                make_reasoning_button(item_index,
                                    std::move(status), at.reasoning,
                                    assistant_metadata(at))
                                    ->Render(),
                                filler(),
                                text(interrupt_hint()) | dim,
                            }),
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
                            if (!follow_ && item_index < _anchor_index) {
                                viewport_.scroll
                                    = std::max(0, viewport_.scroll + delta);
                            }
                            animation::RequestAnimationFrame();
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

            Element content = items.empty() ? text("")
                                            : vbox(std::move(items))
                    | capture_content_height(&viewport_.content_height) | flex;
            Element log     = std::move(content) | vscroll_indicator
                | focusPosition(0,
                    viewport_.scroll
                        + std::max(0, viewport_.viewport_lines() - 1) / 2)
                | yframe;

            Element input_box = panel(vbox({
                separatorEmpty(),
                hbox({
                    text("  "),
                    input_->Render() | xflex,
                    text("  "),
                }),
                separatorEmpty(),
            }));
            Element main      = vbox({
                                    std::move(log) | flex,
                                })
                | flex | reflect(viewport_.box);

            Elements bottom;
            bottom.push_back(
                vbox({
                    hint_bar("↑/↓ scroll · click a card to open in viewer"),
                    hint_bar("Tab next phase · Shift+Tab previous phase"),
                })
                | xflex);
            if (autocomplete_.active()) {
                bottom.push_back(autocomplete_.render(ctx));
            }
            bottom.push_back(vbox({ std::move(input_box) | yflex,
                text("  Alt+Enter add line · @ attach file · $ use skill ")
                    | color(PANEL_FG_DIM) | bgcolor(PANEL_COLOR) }));
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
            if (event == Event::Special("\x1B[200~")) {
                paste_mode_ = true;
                return true;
            }
            if (event == Event::Special("\x1B[201~")) {
                paste_mode_ = false;
                return true;
            }
            if (event == Event::Special("\x1B\r")
                || event == Event::Special("\x1B\n")) {
                insert_newline();
                return true;
            }
            if (event.is_mouse()) {
                if (event.mouse().motion == Mouse::Moved) {
                    hover_dirty_ = true;
                    animation::RequestAnimationFrame();
                }
                for (auto& [id, btn] : read_buttons_) {
                    if (btn->OnEvent(event)) {
                        return true;
                    }
                }
                for (auto& [key, button] : subagent_buttons_) {
                    if (button->OnEvent(event)) {
                        return true;
                    }
                }
                for (auto& [id, link] : reasoning_links_) {
                    if (link.component->OnEvent(event)) {
                        return true;
                    }
                }
            }
            if (event == Event::Escape) {
                if (autocomplete_.active()) {
                    autocomplete_.clear();
                    return true;
                }
                if (!session_->queued().empty()) {
                    session_->cancel_queued(session_->queued().back().id);
                    return true;
                }
                if (session_->phase() != Session::Phase::IDLE) {
                    imza::interrupt(*state_);
                    return true;
                }
                return true;
            }
            if (autocomplete_.active()) {
                if (autocomplete_.handle_event(event)) {
                    return true;
                }
                if (event == Event::Return) {
                    if (!autocomplete_.accept(
                            *state_, input_buf_, input_cursor_, attachments_)) {
                        return true;
                    }
                    submit();
                    return true;
                }
            }
            if (event.is_mouse()) {
                const Mouse& m = event.mouse();
                if (m.button == Mouse::WheelUp) {
                    hover_dirty_ = true;
                    scroll_lines(-kWheelStep);
                    return true;
                }
                if (m.button == Mouse::WheelDown) {
                    hover_dirty_ = true;
                    scroll_lines(kWheelStep);
                    return true;
                }
                return false;
            }
            const bool multiline_input
                = input_buf_.find('\n') != std::string::npos;
            if (!multiline_input) {
                if (event == Event::ArrowUp) {
                    scroll_lines(-1);
                    return true;
                }
                if (event == Event::ArrowDown) {
                    scroll_lines(1);
                    return true;
                }
            }
            if (event == Event::PageUp) {
                scroll_lines(-std::max(1, viewport_lines() - 1));
                return true;
            }
            if (event == Event::PageDown) {
                scroll_lines(std::max(1, viewport_lines() - 1));
                return true;
            }
            if (event == Event::Return) {
                if (paste_mode_) {
                    insert_newline();
                    return true;
                }
                if (input_buf_.empty()) {
                    return true;
                }
                submit();
                return true;
            }
            return input_->OnEvent(event);
        }

        void OnAnimation(animation::Params&) override
        {
            const auto phase = session_->phase();
            if (phase != Session::Phase::STREAMING
                && phase != Session::Phase::CONNECTING) {
                return;
            }
            ++frame_;
            animation::RequestAnimationFrame();
        }

    private:
        int viewport_lines() const { return viewport_.viewport_lines(); }

        void scroll_lines(int delta)
        {
            viewport_.scroll_lines(delta);
            follow_ = viewport_.scroll == viewport_.max_scroll();
        }

        void open_viewer_for(const ToolCall& tc)
        {
            if (tc.name == "read") {
                imza::enqueue_user_modal(*state_,
                    ViewerModal { tool_call_head(tc), tc.result->text,
                        tool_code_language(tc), read_start_line(tc) });
            } else if (tc.name == "skill") {
                imza::enqueue_user_modal(*state_,
                    ViewerModal { tool_call_head(tc), tc.result->text,
                        "markdown", 1, true });
            } else if (tc.name == "list") {
                imza::enqueue_user_modal(*state_,
                    ViewerModal {
                        "Directory listing", tc.result->text, "", 1 });
            } else {
                imza::enqueue_user_modal(*state_,
                    ViewerModal { "Shell output", tc.result->text, "", 1 });
            }
        }

        void open_subagent_viewer(const ToolCall& tc, std::size_t index)
        {
            SubagentChat chat = state_->delegation->subagent_chat(tc, index);
            imza::enqueue_user_modal(*state_,
                ViewerModal { std::move(chat.title), std::move(chat.transcript),
                    "markdown", 1, true, "" });
        }

        void on_input_changed()
        {
            session_->clear_error();
            retain_mentioned_attachments(input_buf_, attachments_);
            autocomplete_.refresh(*state_, input_buf_, input_cursor_);
        }

        void insert_newline()
        {
            input_buf_.insert(input_cursor_, "\n");
            input_cursor_ += 1;
            on_input_changed();
        }

        void submit()
        {
            const std::string text(input_buf_);
            input_buf_.clear();
            input_cursor_ = 0;
            imza::submit(*state_, text, std::move(attachments_));
            attachments_.clear();
            autocomplete_.clear();
            follow_ = true;
            animation::RequestAnimationFrame();
        }

        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        LayoutFn layout_;

        Component container_;
        std::map<std::size_t, Component> read_buttons_;
        std::map<std::pair<std::size_t, std::size_t>, Component>
            subagent_buttons_;
        struct ReasoningLink {
            std::shared_ptr<std::string> label;
            std::shared_ptr<std::string> content;
            std::shared_ptr<std::string> metadata;
            Component component;
        };
        std::map<std::size_t, ReasoningLink> reasoning_links_;

        std::vector<Element> item_cache_;
        std::vector<std::size_t> item_versions_;
        VirtualListState _timeline;
        std::size_t _cached_begin   = 0;
        std::size_t _cached_end     = 0;
        std::size_t _anchor_index   = 0;
        LayoutCtx::Kind cache_kind_ = LayoutCtx::Kind::NARROW;
        int cache_width_            = 0;

        void clear_interaction_cache()
        {
            for (auto& [id, component] : read_buttons_) {
                component->Detach();
            }
            for (auto& [key, component] : subagent_buttons_) {
                component->Detach();
            }
            for (auto& [index, link] : reasoning_links_) {
                link.component->Detach();
            }
            read_buttons_.clear();
            subagent_buttons_.clear();
            reasoning_links_.clear();
        }

        void evict_item(std::size_t index, const ConversationItem& item)
        {
            if (index < item_cache_.size()) {
                item_cache_[index].reset();
                item_versions_[index] = kInvalidVersion;
            }
            if (const auto* tool = std::get_if<ToolCall>(&item)) {
                const auto read = read_buttons_.find(tool->id);
                if (read != read_buttons_.end()) {
                    read->second->Detach();
                    read_buttons_.erase(read);
                }
                auto subagent = subagent_buttons_.lower_bound({ tool->id, 0 });
                while (subagent != subagent_buttons_.end()
                    && subagent->first.first == tool->id) {
                    subagent->second->Detach();
                    subagent = subagent_buttons_.erase(subagent);
                }
            }
            const auto reasoning = reasoning_links_.find(index);
            if (reasoning != reasoning_links_.end()) {
                reasoning->second.component->Detach();
                reasoning_links_.erase(reasoning);
            }
        }

        void evict_range(std::size_t begin, std::size_t end,
            const std::vector<ConversationItem>& conversation)
        {
            end = std::min(end, conversation.size());
            for (std::size_t index = begin; index < end; ++index) {
                evict_item(index, conversation[index]);
            }
        }

        void evict_outside(const VirtualListWindow& window,
            const std::vector<ConversationItem>& conversation)
        {
            evict_range(_cached_begin, std::min(_cached_end, window.begin),
                conversation);
            evict_range(
                std::max(_cached_begin, window.end), _cached_end, conversation);
            _cached_begin = window.begin;
            _cached_end   = window.end;
        }

        Element tool_header_element(const ToolCall& tc)
        {
            Elements parts {
                text(tc.name == "skill" ? "Load Skill"
                                        : tool_display_name(tc.name))
                    | bold | color(HL_GREEN),
                text(" "),
                text(tool_header_args(tc)) | color(PANEL_FG),
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

        Element tool_card(const ToolCall& tc, Element body)
        {
            return vbox({
                tool_header_element(tc),
                std::move(body),
                separatorEmpty(),
            });
        }

        std::string viewer_label(
            std::string what, std::size_t count, std::string_view unit)
        {
            return "▸ open " + std::move(what) + " in viewer ("
                + std::to_string(count) + " " + std::string(unit) + ")";
        }

        const ToolCall* find_tool_call(std::size_t id) const
        {
            for (const ConversationItem& item : session_->items()) {
                if (const auto* call = std::get_if<ToolCall>(&item);
                    call != nullptr && call->id == id) {
                    return call;
                }
            }
            return nullptr;
        }

        Element render_tool_status(
            const ToolCall& tc, std::string marker, ftxui::Color tone)
        {
            return tool_card(tc,
                hbox({
                    text(marker) | bold | color(tone),
                    text(tc.result->text) | color(tone),
                }));
        }

        Element render_skill_item(const ToolCall& tc)
        {
            const std::string label = viewer_label(
                "skill instructions", count_lines(tc.result->text), "lines");
            Component btn = make_viewer_button(tc.id, label);
            return tool_card(tc, btn->Render());
        }

        Element render_read_item(const ToolCall& tc)
        {
            const std::string label = viewer_label(
                tool_call_head(tc), count_lines(tc.result->text), "lines");
            Component btn = make_viewer_button(tc.id, label);
            return tool_card(tc, btn->Render());
        }

        Element render_list_collapsed(const ToolCall& tc)
        {
            const std::string& full = tc.result->text;
            std::size_t entries     = count_lines(full);
            if (full.find("\n[truncated:") != std::string::npos) {
                --entries;
            }
            const std::string label
                = viewer_label("directory listing", entries, "entries");
            Component btn = make_viewer_button(tc.id, label);
            return tool_card(tc, btn->Render());
        }

        Element render_shell_collapsed(const ToolCall& tc)
        {
            const std::string& full   = tc.result->text;
            const std::size_t total   = count_lines(full);
            const std::string preview = take_lines(full, kLargeOutputLines);
            const std::string label
                = viewer_label("full shell output", total, "lines");
            Component btn = make_viewer_button(tc.id, label);
            return tool_card(
                tc, vbox({ code_block(preview, ""), btn->Render() }));
        }

        Element render_shell_item(const ToolCall& tc)
        {
            Elements parts { tool_header_element(tc) };
            if (!tc.result->text.empty()) {
                parts.push_back(code_block(tc.result->text, ""));
            }
            parts.push_back(separatorEmpty());
            return vbox(std::move(parts));
        }

        Element render_write_item(const ToolCall& tc)
        {
            Element body;
            Element header = tool_header_element(tc);
            if (tc.result->diff.has_value()) {
                const DiffView& diff  = *tc.result->diff;
                std::size_t additions = 0;
                std::size_t deletions = 0;
                for (const DiffRow& row : diff.rows) {
                    deletions += diff_row_left_changed(row) ? 1 : 0;
                    additions += diff_row_right_changed(row) ? 1 : 0;
                }
                header              = hbox({ std::move(header), filler(),
                    diffstat_chip(additions, deletions) });
                const LayoutCtx ctx = layout_();
                body = diff_split(diff, review_content_width(ctx));
            } else {
                body = code_block(tc.result->text, tool_code_language(tc));
            }
            return vbox({
                std::move(header),
                body,
                separatorEmpty(),
            });
        }

        Element render_ask_item(const ToolCall& tc)
        {
            return tool_card(tc, render_markdown_element(tc.result->text));
        }

        Element render_web_item(const ToolCall& tc)
        {
            Element status = text("done") | dim;
            if (!tc.result.has_value()) {
                status = hbox({
                    spinner(15, static_cast<std::size_t>(frame_)) | dim,
                    text(" …") | dim,
                });
            } else if (tc.result->kind == ToolCall::Result::Kind::ERROR) {
                status = text("failed") | color(HL_RED);
            }
            return vbox({
                hbox({
                    tool_header_element(tc),
                    filler(),
                    std::move(status),
                }),
                separatorEmpty(),
            });
        }

        Element render_generic_tool(const ToolCall& tc)
        {
            return tool_card(tc, code_block(tc.result->text, ""));
        }

        Element render_tool_error(const ToolCall& tc)
        {
            return render_tool_status(tc, "Error: ", HL_RED);
        }

        Element render_tool_reject(const ToolCall& tc)
        {
            return render_tool_status(tc, "Rejected: ", HL_YELLOW);
        }

        Element render_tool_pending(const ToolCall& tc)
        {
            if (tc.name == "subagent") {
                Elements rows {
                    hbox({
                        spinner(15, static_cast<std::size_t>(frame_))
                            | color(PANEL_FG_DIM),
                        text(" Delegating…" + elapsed_suffix(*session_)) | dim,
                    }),
                };
                for (std::size_t index = 0; index < tc.subagent_ids.size();
                    ++index) {
                    const SubagentChat chat
                        = state_->delegation->subagent_chat(tc, index);
                    rows.push_back(make_subagent_viewer_button(
                        tc.id, index, "‹ View " + chat.title + " chat ›")
                            ->Render());
                }
                rows.push_back(separatorEmpty());
                return vbox(std::move(rows));
            }
            if (tc.name == "shell") {
                return vbox({
                    hbox({
                        spinner(15, static_cast<std::size_t>(frame_))
                            | color(HL_GREEN),
                        text(" "),
                        tool_header_element(tc),
                    }),
                    separatorEmpty(),
                });
            }
            return vbox({
                tool_header_element(tc),
                separatorEmpty(),
            });
        }

        Element render_subagent_item(const ToolCall& tc)
        {
            Elements rows { tool_header_element(tc) };
            const std::size_t count
                = std::max(tc.subagent_ids.size(), tc.subagent_chats.size());
            for (std::size_t index = 0; index < count; ++index) {
                const SubagentChat chat
                    = state_->delegation->subagent_chat(tc, index);
                rows.push_back(make_subagent_viewer_button(
                    tc.id, index, "‹ View " + chat.title + " chat ›")
                        ->Render());
            }
            rows.push_back(separatorEmpty());
            return vbox(std::move(rows));
        }

        template <typename Key>
        Component memoized_label_button(std::map<Key, Component>& cache,
            Key key, std::string label, std::function<void()> on_click)
        {
            if (const auto found = cache.find(key); found != cache.end()) {
                return found->second;
            }
            auto shared_label
                = std::make_shared<const std::string>(std::move(label));
            Component btn = inline_link_button(
                [shared_label] { return text(*shared_label); },
                std::move(on_click), PANEL_FG_DIM);
            cache.emplace(std::move(key), btn);
            container_->Add(btn);
            return btn;
        }

        Component make_subagent_viewer_button(
            std::size_t id, std::size_t index, std::string label)
        {
            return memoized_label_button(subagent_buttons_,
                std::pair { id, index }, std::move(label), [this, id, index] {
                    if (const auto* call = find_tool_call(id);
                        call != nullptr) {
                        open_subagent_viewer(*call, index);
                    }
                });
        }

        Component make_viewer_button(std::size_t id, std::string label)
        {
            return memoized_label_button(
                read_buttons_, id, std::move(label), [this, id] {
                    if (const auto* tc = find_tool_call(id); tc != nullptr) {
                        open_viewer_for(*tc);
                    }
                });
        }

        bool reasoning_enabled(const AssistantTurn& turn) const
        {
            return !turn.reasoning_effort.empty()
                && turn.reasoning_effort != "off";
        }

        Element render_assistant(const AssistantTurn& t, std::size_t index,
            const LayoutCtx&, bool active, bool show_metadata)
        {
            Elements parts;
            const bool has_reasoning = !t.reasoning.empty();
            const bool expected      = reasoning_enabled(t);
            const bool done          = t.reasoning_ms.has_value();
            const bool placeholder
                = active && !has_reasoning && !done && expected;
            if (has_reasoning || placeholder || (done && expected)) {
                std::string label;
                if (done) {
                    const double secs
                        = static_cast<double>(t.reasoning_ms->count()) / 1000.0;
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f", secs);
                    label = "▸ Thought " + std::string(buf) + "s";
                } else {
                    label = " Thinking…" + elapsed_suffix(*session_);
                }
                if (done && !has_reasoning) {
                    parts.push_back(text(label) | dim);
                } else {
                    Component btn = make_reasoning_button(index, label,
                        placeholder ? std::string() : t.reasoning,
                        assistant_metadata(t));
                    Element row   = done
                        ? btn->Render()
                        : hbox({ spinner(15, static_cast<size_t>(frame_))
                                  | color(PANEL_FG_DIM),
                              btn->Render(), filler(),
                              text(interrupt_hint()) | dim });
                    parts.push_back(row);
                }
            }
            if (!t.markdown.empty()) {
                parts.push_back(assistant_item(t));
            }
            if (show_metadata) {
                parts.push_back(hint_bar(assistant_metadata(t)));
            }
            return vbox(std::move(parts));
        }

        Component make_reasoning_button(std::size_t index, std::string label,
            const std::string& content, std::string metadata)
        {
            if (auto it = reasoning_links_.find(index);
                it != reasoning_links_.end()) {
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
                        ViewerModal vm { " Thinking", *content_ptr, "", 1 };
                        vm.line_numbers = false;
                        vm.metadata     = *metadata_ptr;
                        enqueue_user_modal(*state_, vm);
                    },
                    PANEL_FG_DIM);
            container_->Add(entry.component);
            reasoning_links_.emplace(index, std::move(entry));
            return reasoning_links_.find(index)->second.component;
        }

        std::string interrupt_hint() { return "Esc interrupt"; }

        std::string input_buf_;
        InputOption input_options_;
        Component input_;

        Autocomplete autocomplete_;
        std::vector<FileAttachment> attachments_;
        int input_cursor_ = 0;
        bool paste_mode_  = false;

        bool follow_                  = true;
        bool hover_dirty_             = false;
        int frame_                    = 0;
        std::uint64_t content_serial_ = 0;
        ScrollView viewport_ { };
    };

} // namespace

ftxui::Element render_item(const ConversationItem& item, const LayoutCtx& ctx)
{
    return std::visit(
        [&](const auto& v) -> Element {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, UserTurn>) {
                return user_item(v);
            } else if constexpr (std::is_same_v<T, AssistantTurn>) {
                return assistant_item(v);
            } else if constexpr (std::is_same_v<T, TodoList>) {
                return render_todo(v, ctx);
            } else if constexpr (std::is_same_v<T, ModalAnswer>) {
                return modal_answer_item(v);
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
    std::shared_ptr<ApplicationState> state, LayoutFn layout)
{
    return ftxui::Make<ChatImpl>(std::move(state), std::move(layout));
}

} // namespace imza
