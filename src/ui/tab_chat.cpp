#include "agent/flows.h"
#include "common/util.h"
#include "subsystems/attachments.h"
#include "subsystems/delegation_runner.h"
#include "subsystems/format.h"
#include "ui/autocomplete.h"
#include "ui/tool_format.h"
#include "ui/ui.h"

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

namespace ursa {

using namespace ftxui;

namespace {

    using namespace ftxui;

    constexpr std::size_t kLargeOutputLines = 10;
    constexpr std::size_t kInvalidVersion   = ~std::size_t { 0 };
    constexpr int kWheelStep                = 3;

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

            Elements items;
            if (follow_) {
                viewport_.scroll = viewport_.max_scroll();
            } else {
                viewport_.scroll_lines(0);
            }
            const std::uint64_t content_serial = st.content_serial();
            const std::vector<ConversationItem>& conversation = st.items();
            const std::size_t item_count = conversation.size();
            if (cache_kind_ != ctx.kind || cache_width_ != ctx.width
                || content_serial_ != content_serial
                || item_cache_.size() != item_count) {
                item_cache_.clear();
                item_cache_.resize(item_count);
                item_versions_.assign(item_count, kInvalidVersion);
                cache_kind_     = ctx.kind;
                cache_width_    = ctx.width;
                content_serial_ = content_serial;
            }
            if (std::exchange(hover_dirty_, false)) {
                item_versions_.assign(item_cache_.size(), kInvalidVersion);
            }
            std::size_t item_index = 0;
            for (const auto& it : conversation) {
                if (item_index >= item_cache_.size()) {
                    break;
                }
                const std::size_t version = item_version(it);
                const bool is_trailing    = item_index + 1 == item_count;
                const bool active         = is_trailing && streaming
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
                            | color(Color::GrayLight),
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
                                    | color(Color::GrayLight),
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
                items.push_back(hbox({
                    text(" "),
                    std::move(el) | xflex,
                }));
                ++item_index;
            }

            const size_t queued_n = st.queued().size();
            for (size_t i = 0; i < queued_n; ++i) {
                const auto& q = st.queued()[i];
                Elements row {
                    text("[QUEUED] ") | bold | color(Color::Green),
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
                    hbox({
                        filler(),
                        text("↑/↓ scroll · click a card to open in viewer")
                            | color(PANEL_FG_DIM),
                        text(" "),
                    }),
                    hbox({
                        filler(),
                        text("Tab next phase · Shift+Tab previous phase")
                            | color(PANEL_FG_DIM),
                        text(" "),
                    }),
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
                for (auto& [id, btn] : reasoning_comps_) {
                    if (btn->OnEvent(event)) {
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
                    ursa::interrupt(*state_);
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
                ursa::enqueue_user_modal(*state_,
                    ViewerModal { tool_call_head(tc), tc.result->text,
                        tool_code_language(tc), read_start_line(tc) });
            } else if (tc.name == "skill") {
                ursa::enqueue_user_modal(*state_,
                    ViewerModal { tool_call_head(tc), tc.result->text,
                        "markdown", 1, true });
            } else if (tc.name == "list") {
                ursa::enqueue_user_modal(*state_,
                    ViewerModal {
                        "Directory listing", tc.result->text, "", 1 });
            } else {
                ursa::enqueue_user_modal(*state_,
                    ViewerModal { "Shell output", tc.result->text, "", 1 });
            }
        }

        void open_subagent_viewer(const ToolCall& tc, std::size_t index)
        {
            SubagentChat chat = state_->delegation->subagent_chat(tc, index);
            ursa::enqueue_user_modal(*state_,
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
            ursa::submit(*state_, text, std::move(attachments_));
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
        std::map<std::size_t, std::shared_ptr<std::string>> reasoning_labels_;
        std::map<std::size_t, std::shared_ptr<std::string>> reasoning_content_;
        std::map<std::size_t, std::shared_ptr<std::string>> reasoning_metadata_;
        std::map<std::size_t, Component> reasoning_comps_;

        std::vector<Element> item_cache_;
        std::vector<std::size_t> item_versions_;
        LayoutCtx::Kind cache_kind_ = LayoutCtx::Kind::NARROW;
        int cache_width_            = 0;

        Element tool_header_element(const ToolCall& tc)
        {
            Elements parts {
                text(tc.name == "skill" ? "Load Skill"
                                        : tool_display_name(tc.name))
                    | bold | color(Color::GreenLight),
                text(" "),
                text(tool_header_args(tc)) | color(PANEL_FG),
            };
            if (tc.result.has_value() && tc.result->shell_status.has_value()) {
                const std::string status
                    = shell_status_text(*tc.result->shell_status);
                if (!status.empty()) {
                    parts.push_back(filler());
                    parts.push_back(text(status) | color(Color::RedLight));
                }
            }
            return hbox(std::move(parts));
        }

        Element render_skill_item(const ToolCall& tc)
        {
            const std::string label = "▸ open skill instructions in viewer ("
                + std::to_string(count_lines(tc.result->text)) + " lines)";
            Component btn = make_viewer_button(tc.id, label);
            return vbox({
                tool_header_element(tc),
                btn->Render(),
                separatorEmpty(),
            });
        }

        Element render_read_item(const ToolCall& tc)
        {
            const std::string content = tc.result->text;
            const std::string label   = "▸ open " + tool_call_head(tc)
                + " in viewer (" + std::to_string(count_lines(content))
                + " lines)";
            Component btn = make_viewer_button(tc.id, label);
            return vbox({
                tool_header_element(tc),
                btn->Render(),
                separatorEmpty(),
            });
        }

        Element render_list_collapsed(const ToolCall& tc)
        {
            const std::string& full = tc.result->text;
            std::size_t entries     = 0;
            for (const auto& line : split_lines(full)) {
                if (!line.empty() && line.rfind("[truncated", 0) != 0) {
                    ++entries;
                }
            }
            const std::string label = "▸ open directory listing in viewer ("
                + std::to_string(entries) + " entries)";
            Component btn = make_viewer_button(tc.id, label);
            return vbox({
                tool_header_element(tc),
                btn->Render(),
                separatorEmpty(),
            });
        }

        Element render_shell_collapsed(const ToolCall& tc)
        {
            const std::string& full   = tc.result->text;
            const std::size_t total   = count_lines(full);
            const std::string preview = take_lines(full, kLargeOutputLines);
            const std::string label   = "▸ open full shell output in viewer ("
                + std::to_string(total) + " lines)";
            Component btn = make_viewer_button(tc.id, label);
            return vbox({
                tool_header_element(tc),
                code_block(preview, ""),
                btn->Render(),
                separatorEmpty(),
            });
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
                    text("+" + std::to_string(additions))
                        | color(Color::GreenLight),
                    text(" "),
                    text("−" + std::to_string(deletions))
                        | color(Color::RedLight) });
                const LayoutCtx ctx = layout_();
                const int width     = ctx.kind == LayoutCtx::Kind::WIDE
                    ? ctx.width - LayoutCtx::panel_width - 4
                    : ctx.width;
                body                = diff_split(diff, width);
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
            return vbox({
                tool_header_element(tc),
                render_markdown_element(tc.result->text),
                separatorEmpty(),
            });
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
                status = text("failed") | color(Color::RedLight);
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
            return vbox({
                tool_header_element(tc),
                code_block(tc.result->text, ""),
                separatorEmpty(),
            });
        }

        Element render_tool_error(const ToolCall& tc)
        {
            return vbox({
                tool_header_element(tc),
                hbox({
                    text("Error: ") | bold | color(Color::RedLight),
                    text(tc.result->text) | color(Color::RedLight),
                }),
                separatorEmpty(),
            });
        }

        Element render_tool_reject(const ToolCall& tc)
        {
            return vbox({
                tool_header_element(tc),
                hbox({
                    text("Rejected: ") | bold | color(Color::YellowLight),
                    text(tc.result->text) | color(Color::YellowLight),
                }),
                separatorEmpty(),
            });
        }

        Element render_tool_pending(const ToolCall& tc)
        {
            if (tc.name == "subagent") {
                Elements rows {
                    hbox({
                        spinner(15, static_cast<std::size_t>(frame_))
                            | color(Color::GrayLight),
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
                            | color(Color::GreenLight),
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
                    for (const ConversationItem& item : session_->items()) {
                        const auto* call = std::get_if<ToolCall>(&item);
                        if (call != nullptr && call->id == id) {
                            open_subagent_viewer(*call, index);
                            return;
                        }
                    }
                });
        }

        Component make_viewer_button(std::size_t id, std::string label)
        {
            return memoized_label_button(
                read_buttons_, id, std::move(label), [this, id] {
                    for (const auto& item : session_->items()) {
                        const auto* tc = std::get_if<ToolCall>(&item);
                        if (tc != nullptr && tc->id == id) {
                            open_viewer_for(*tc);
                            return;
                        }
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
                                  | color(Color::GrayLight),
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
            auto it = reasoning_comps_.find(index);
            if (it != reasoning_comps_.end()) {
                *reasoning_labels_[index]   = label;
                *reasoning_content_[index]  = content;
                *reasoning_metadata_[index] = std::move(metadata);
                return it->second;
            }
            auto label_ptr   = std::make_shared<std::string>(std::move(label));
            auto content_ptr = std::make_shared<std::string>(content);
            auto metadata_ptr
                = std::make_shared<std::string>(std::move(metadata));
            const auto on_click = [this, content_ptr, metadata_ptr] {
                ViewerModal vm { " Thinking", *content_ptr, "", 1 };
                vm.line_numbers = false;
                vm.metadata     = *metadata_ptr;
                ursa::enqueue_user_modal(*state_, vm);
            };
            Component btn
                = inline_link_button([label_ptr] { return text(*label_ptr); },
                    on_click, PANEL_FG_DIM);
            reasoning_labels_[index]   = label_ptr;
            reasoning_content_[index]  = content_ptr;
            reasoning_metadata_[index] = metadata_ptr;
            reasoning_comps_[index]    = btn;
            container_->Add(btn);
            return btn;
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

} // namespace ursa
