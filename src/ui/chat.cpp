#include "ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "attachments.h"
#include "format.h"
#include "util.h"

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

    // reflect() clips the recorded box to the screen stencil at render time,
    // which under a yframe reports the viewport height instead of the content
    // height. This captures the unclipped requirement instead.
    Decorator content_height(int* out)
    {
        class Impl : public Node {
        public:
            Impl(Element child, int* out)
                : Node(Elements { std::move(child) })
                , out_(out)
            {
            }

            void ComputeRequirement() override
            {
                Node::ComputeRequirement();
                requirement_ = children_[0]->requirement();
                *out_        = requirement_.min_y;
            }

            void SetBox(Box box) override
            {
                Node::SetBox(box);
                children_[0]->SetBox(box);
            }

        private:
            int* out_;
        };
        return [out](Element child) {
            return std::make_shared<Impl>(std::move(child), out);
        };
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

    Element error_element(const Session& st)
    {
        std::string msg = st.error();
        if (st.retry_countdown()) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                st.retry_countdown()->deadline
                - std::chrono::steady_clock::now())
                                 .count();
            if (remaining < 0) {
                remaining = 0;
            }
            msg = "rate limited — retrying in " + std::to_string(remaining)
                + "s…";
        }
        if (msg.empty()) {
            return text("");
        }
        return hbox({
            text(" ") | bgcolor(Color::Red),
            text(" " + msg) | bgcolor(Color::Red),
            filler() | bgcolor(Color::Red),
        });
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
        ChatImpl(std::shared_ptr<Session> session, Controller& controller,
            LayoutFn layout)
            : session_(std::move(session))
            , controller_(controller)
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

            const bool streaming     = st.phase() == Session::Phase::STREAMING;
            const bool connecting    = st.phase() == Session::Phase::CONNECTING;
            const bool busy          = streaming || connecting;
            const bool shell_running = busy
                && std::any_of(st.items().begin(), st.items().end(),
                    [](const ConversationItem& item) {
                        const auto* tc = std::get_if<ToolCall>(&item);
                        return tc != nullptr && tc->name == "shell"
                            && !tc->result.has_value();
                    });
            const bool compaction_running = std::any_of(st.items().begin(),
                st.items().end(), [](const ConversationItem& item) {
                    const auto* event = std::get_if<CompactionEvent>(&item);
                    return event != nullptr
                        && event->status == CompactionEvent::Status::RUNNING;
                });
            if (shell_running || compaction_running) {
                animation::RequestAnimationFrame();
            }

            Elements items;
            if (follow_) {
                scroll_top_ = max_scroll();
            } else {
                scroll_top_ = std::clamp(scroll_top_, 0, max_scroll());
            }
            const std::uint64_t content_serial = st.content_serial();
            if (cache_kind_ != ctx.kind || content_serial_ != content_serial
                || item_cache_.size() != st.items().size()) {
                item_cache_.clear();
                item_cache_.resize(st.items().size());
                item_versions_.assign(st.items().size(), kInvalidVersion);
                cache_kind_     = ctx.kind;
                content_serial_ = content_serial;
            }
            std::size_t item_index = 0;
            for (const auto& it : st.items()) {
                const std::size_t version = item_version(it);
                const bool is_trailing    = &it == &st.items().back();
                const bool active         = is_trailing && streaming
                    && std::holds_alternative<AssistantTurn>(it);
                const bool final_segment = !(is_trailing && busy)
                    && (item_index + 1 == st.items().size()
                        || std::holds_alternative<UserTurn>(
                            st.items()[item_index + 1]));
                std::size_t eff_version = version;
                if (const auto* tc = std::get_if<ToolCall>(&it); tc != nullptr
                    && tc->name == "shell" && !tc->result.has_value()) {
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
                                if (tc.name == "read") {
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
                    && &it == &st.items().back()) {
                    const auto& at = std::get<AssistantTurn>(it);
                    if (at.reasoning.empty()
                        && (!reasoning_enabled(at) || connecting)) {
                        el = vbox({
                            hbox({
                                spinner(15, static_cast<size_t>(frame_))
                                    | color(Color::GrayLight),
                                text(connecting ? " Connecting…" : " Thinking…")
                                    | dim,
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
                    | content_height(&content_height_) | flex;
            Element log     = std::move(content) | vscroll_indicator
                | focusPosition(
                    0, scroll_top_ + std::max(0, viewport_lines() - 1) / 2)
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
                | flex | reflect(frame_box_);

            Elements bottom;
            bottom.push_back(
                vbox({
                    hbox({
                        filler(),
                        text("↑/↓ scroll · click a card to open in viewer")
                            | color(PANEL_FG_DIM),
                        text(" "),
                    }),
                })
                | xflex);
            if (show_suggestions()) {
                bottom.push_back(render_suggestions());
            }
            bottom.push_back(vbox({ std::move(input_box) | yflex,
                text("  Tab switch mode · Alt+Enter add line · @ attach file · "
                     "$ "
                     "use skill ")
                    | color(PANEL_FG_DIM) | bgcolor(PANEL_COLOR) }));
            if (!st.error().empty() || st.retry_countdown()) {
                bottom.push_back(error_element(st));
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
            if (event.is_mouse() && event.mouse().button == Mouse::Left
                && event.mouse().motion == Mouse::Pressed) {
                for (auto& [id, btn] : read_buttons_) {
                    if (btn->OnEvent(event)) {
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
                if (show_suggestions()) {
                    matches_.clear();
                    skill_matches_.clear();
                    file_matches_.clear();
                    attachment_token_.reset();
                    return true;
                }
                if (!session_->queued().empty()) {
                    controller_.cancel_queued(session_->queued().back().id);
                    return true;
                }
                if (session_->phase() != Session::Phase::IDLE) {
                    controller_.interrupt();
                    return true;
                }
                return true;
            }
            if (show_suggestions()) {
                if (event == Event::ArrowDown) {
                    sel_ = (sel_ + 1) % suggestion_count();
                    return true;
                }
                if (event == Event::ArrowUp) {
                    const int n = suggestion_count();
                    sel_        = (sel_ - 1 + n) % n;
                    return true;
                }
                if (event == Event::Tab) {
                    accept();
                    return true;
                }
                if (event == Event::Return) {
                    const bool insertion_suggestion
                        = !file_matches_.empty() || !skill_matches_.empty();
                    accept();
                    if (!insertion_suggestion) {
                        submit();
                    }
                    return true;
                }
            }
            if (event == Event::Tab && !show_suggestions()) {
                controller_.toggle_mode();
                return true;
            }
            if (event.is_mouse()) {
                const Mouse& m = event.mouse();
                if (m.button == Mouse::WheelUp) {
                    scroll_lines(-kWheelStep);
                    return true;
                }
                if (m.button == Mouse::WheelDown) {
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
        int content_lines() const { return content_height_; }

        int viewport_lines() const
        {
            if (frame_box_.y_max < frame_box_.y_min) {
                return 0;
            }
            return frame_box_.y_max - frame_box_.y_min + 1;
        }

        int max_scroll() const
        {
            return std::max(0, content_lines() - viewport_lines());
        }

        void scroll_lines(int delta)
        {
            scroll_top_ = std::clamp(scroll_top_ + delta, 0, max_scroll());
            follow_     = scroll_top_ == max_scroll();
        }

        void open_viewer_for(const ToolCall& tc)
        {
            if (tc.name == "read") {
                controller_.enqueue_user_modal(
                    ViewerModal { tool_call_head(tc), tc.result->text,
                        tool_code_language(tc), read_start_line(tc) });
            } else if (tc.name == "skill") {
                controller_.enqueue_user_modal(ViewerModal {
                    tool_call_head(tc), tc.result->text, "markdown", 1, true });
            } else if (tc.name == "list") {
                controller_.enqueue_user_modal(ViewerModal {
                    "Directory listing", tc.result->text, "", 1 });
            } else {
                controller_.enqueue_user_modal(
                    ViewerModal { "Shell output", tc.result->text, "", 1 });
            }
        }

        void on_input_changed()
        {
            controller_.clear_error();
            retain_mentioned_attachments(input_buf_, attachments_);
            refresh_suggestions();
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
            controller_.submit(std::move(text), std::move(attachments_));
            attachments_.clear();
            follow_ = true;
            animation::RequestAnimationFrame();
        }

        void refresh_suggestions()
        {
            matches_.clear();
            skill_matches_.clear();
            file_matches_.clear();
            attachment_token_.reset();
            sel_                   = 0;
            const std::string text = input_buf_;
            attachment_token_      = attachment_token_at(
                text, static_cast<std::size_t>(input_cursor_));
            if (attachment_token_) {
                file_matches_ = attachment_candidates(
                    std::filesystem::current_path(), attachment_token_->query);
                return;
            }
            std::size_t token_begin = static_cast<std::size_t>(input_cursor_);
            while (token_begin > 0
                && !std::isspace(
                    static_cast<unsigned char>(text[token_begin - 1]))) {
                --token_begin;
            }
            if (token_begin < static_cast<std::size_t>(input_cursor_)
                && text[token_begin] == '$') {
                skill_token_begin_    = token_begin;
                const std::string key = to_lower(text.substr(token_begin + 1,
                    static_cast<std::size_t>(input_cursor_) - token_begin - 1));
                for (const Skill& skill : controller_.available_skills()) {
                    const std::string name = to_lower(skill.name);
                    if (name.starts_with(key))
                        skill_matches_.push_back(skill);
                }
                return;
            }
            skill_token_begin_.reset();
            if (text.empty() || text[0] != '/'
                || text.find(' ') != std::string::npos) {
                return;
            }
            const std::string key = to_lower(text);
            for (const auto& c : controller_.commands()) {
                const std::string name = to_lower(c.name);
                if (name.size() >= key.size()
                    && name.compare(0, key.size(), key) == 0) {
                    matches_.push_back(&c);
                }
            }
            if (matches_.size() == 1 && matches_[0]->name == text) {
                matches_.clear();
            }
        }

        void accept()
        {
            if (!file_matches_.empty() && attachment_token_) {
                const AttachmentCandidate& candidate = file_matches_[sel_];
                const std::string replacement        = "@" + candidate.path;
                input_buf_.replace(attachment_token_->begin,
                    attachment_token_->end - attachment_token_->begin,
                    replacement);
                input_cursor_ = static_cast<int>(
                    attachment_token_->begin + replacement.size());
                if (candidate.directory) {
                    refresh_suggestions();
                    return;
                }
                AttachmentResult loaded = load_attachment(
                    std::filesystem::current_path(), candidate.path);
                if (!loaded.attachment) {
                    controller_.set_error(std::move(loaded.error));
                    refresh_suggestions();
                    return;
                }
                const auto duplicate = std::find_if(attachments_.begin(),
                    attachments_.end(), [&](const FileAttachment& attachment) {
                        return attachment.path == loaded.attachment->path;
                    });
                if (duplicate == attachments_.end()) {
                    constexpr std::size_t kMaxAttachments = 20;
                    constexpr std::size_t kMaxTotalBytes  = 4 * 1024 * 1024;
                    std::size_t total = loaded.attachment->content.size();
                    for (const auto& attachment : attachments_) {
                        total += attachment.content.size();
                    }
                    if (attachments_.size() >= kMaxAttachments
                        || total > kMaxTotalBytes) {
                        controller_.set_error("attachments exceed the 20 file "
                                              "or 4 MiB total limit");
                        refresh_suggestions();
                        return;
                    }
                    attachments_.push_back(std::move(*loaded.attachment));
                }
                input_buf_.insert(input_cursor_, " ");
                ++input_cursor_;
                matches_.clear();
                file_matches_.clear();
                attachment_token_.reset();
                return;
            }
            if (!skill_matches_.empty() && skill_token_begin_) {
                const std::string replacement
                    = "$" + skill_matches_[static_cast<std::size_t>(sel_)].name;
                input_buf_.replace(*skill_token_begin_,
                    static_cast<std::size_t>(input_cursor_)
                        - *skill_token_begin_,
                    replacement);
                input_cursor_ = static_cast<int>(
                    *skill_token_begin_ + replacement.size());
                input_buf_.insert(static_cast<std::size_t>(input_cursor_), " ");
                ++input_cursor_;
                refresh_suggestions();
                return;
            }
            const SlashCommand* cmd = matches_[sel_];
            input_buf_              = cmd->name;
            input_cursor_           = static_cast<int>(input_buf_.size());
            refresh_suggestions();
        }

        int suggestion_count() const
        {
            if (!file_matches_.empty())
                return static_cast<int>(file_matches_.size());
            if (!skill_matches_.empty())
                return static_cast<int>(skill_matches_.size());
            return static_cast<int>(matches_.size());
        }

        bool show_suggestions() const { return suggestion_count() > 0; }

        Element render_suggestions()
        {
            const size_t max_rows     = 8;
            const LayoutCtx ctx       = layout_();
            const int available_width = ctx.kind == LayoutCtx::Kind::WIDE
                ? ctx.width - LayoutCtx::panel_width
                : ctx.width;
            const int description_width
                = std::clamp(available_width - 28, 8, 56);
            const size_t total = !file_matches_.empty() ? file_matches_.size()
                : !skill_matches_.empty()               ? skill_matches_.size()
                                                        : matches_.size();
            const size_t shown = std::min(total, max_rows);
            const size_t selected = static_cast<size_t>(std::max(0, sel_));
            const size_t first    = selected < shown
                ? 0
                : std::min(selected - shown + 1, total - shown);
            Elements rows;
            if (first > 0) {
                rows.push_back(text("  ↑ " + std::to_string(first) + " more")
                    | dim | color(PANEL_FG_DIM));
            }
            for (size_t row_index = 0; row_index < shown; ++row_index) {
                const size_t i              = first + row_index;
                const bool sel              = static_cast<int>(i) == sel_;
                const std::string name_text = !file_matches_.empty()
                    ? "@" + file_matches_[i].path
                    : !skill_matches_.empty() ? "$" + skill_matches_[i].name
                                              : std::string(matches_[i]->name);
                Element name                = text(name_text);
                if (sel) {
                    name = name | bold;
                }
                const std::string description = !file_matches_.empty()
                    ? (file_matches_[i].directory ? "directory" : "file")
                    : !skill_matches_.empty() ? skill_matches_[i].description
                                              : std::string(matches_[i]->desc);
                Element row                   = hbox({
                    std::move(name) | xflex,
                    text("  "),
                    text(fit(description, description_width)) | dim
                        | color(PANEL_FG_DIM),
                });
                row                           = row | xflex
                    | (sel ? bgcolor(PANEL_COLOR_FOCUS) : bgcolor(PANEL_COLOR));
                rows.push_back(std::move(row));
            }
            const size_t remaining = total - first - shown;
            if (remaining > 0) {
                rows.push_back(
                    text("  ↓ " + std::to_string(remaining) + " more") | dim
                    | color(PANEL_FG_DIM));
            }
            return vbox(std::move(rows)) | xflex | bgcolor(PANEL_COLOR)
                | color(PANEL_FG);
        }

        std::shared_ptr<Session> session_;
        Controller& controller_;
        LayoutFn layout_;

        Component container_;
        std::map<std::size_t, Component> read_buttons_;
        std::map<std::size_t, std::shared_ptr<std::string>> reasoning_labels_;
        std::map<std::size_t, std::shared_ptr<std::string>> reasoning_content_;
        std::map<std::size_t, std::shared_ptr<std::string>> reasoning_metadata_;
        std::map<std::size_t, Component> reasoning_comps_;

        std::vector<Element> item_cache_;
        std::vector<std::size_t> item_versions_;
        LayoutCtx::Kind cache_kind_ = LayoutCtx::Kind::NARROW;

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
            if (tc.result->diff.has_value()) {
                body = diff_split(*tc.result->diff);
            } else {
                body = code_block(tc.result->text, tool_code_language(tc));
            }
            return vbox({
                tool_header_element(tc),
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

        Component make_viewer_button(std::size_t id, std::string label)
        {
            auto it = read_buttons_.find(id);
            if (it != read_buttons_.end()) {
                return it->second;
            }
            const auto on_click = [this, id] {
                for (const auto& item : session_->items()) {
                    const auto* tc = std::get_if<ToolCall>(&item);
                    if (tc != nullptr && tc->id == id) {
                        open_viewer_for(*tc);
                        return;
                    }
                }
            };
            ButtonOption bo;
            bo.label     = std::move(label);
            bo.on_click  = on_click;
            bo.transform = [](EntryState s) -> Element {
                Element e = text(s.label);
                if (s.focused) {
                    e = e | bold | underlined | color(PANEL_FG);
                } else {
                    e = e | color(PANEL_FG_DIM);
                }
                return e;
            };
            Component btn = space_activates(Button(bo), on_click);
            read_buttons_.emplace(id, btn);
            container_->Add(btn);
            return btn;
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
            const bool placeholder   = active && !has_reasoning
                && !t.reasoning_ms.has_value() && reasoning_enabled(t);
            if (has_reasoning || placeholder) {
                const bool done = t.reasoning_ms.has_value();
                std::string label;
                if (done) {
                    const double secs
                        = static_cast<double>(t.reasoning_ms->count()) / 1000.0;
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f", secs);
                    label = "▸ Thought " + std::string(buf) + "s";
                } else {
                    label = " Thinking…";
                }
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
            if (!t.markdown.empty()) {
                parts.push_back(assistant_item(t));
            }
            if (show_metadata) {
                parts.push_back(
                    hbox({ filler(), text(assistant_metadata(t)) | dim }));
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
                controller_.enqueue_user_modal(vm);
            };
            ButtonOption bo;
            bo.label     = *label_ptr;
            bo.on_click  = on_click;
            bo.transform = [label_ptr](EntryState s) -> Element {
                Element e = text(*label_ptr);
                if (s.focused) {
                    e = e | bold | underlined | color(PANEL_FG);
                } else {
                    e = e | color(PANEL_FG_DIM);
                }
                return e;
            };
            Component btn              = space_activates(Button(bo), on_click);
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

        std::vector<const SlashCommand*> matches_;
        std::vector<AttachmentCandidate> file_matches_;
        std::optional<AttachmentToken> attachment_token_;
        std::vector<FileAttachment> attachments_;
        std::vector<Skill> skill_matches_;
        std::optional<std::size_t> skill_token_begin_;
        int sel_          = 0;
        int input_cursor_ = 0;
        bool paste_mode_  = false;

        int scroll_top_               = 0;
        bool follow_                  = true;
        int frame_                    = 0;
        int content_height_           = 0;
        std::uint64_t content_serial_ = 0;
        ftxui::Box frame_box_ { };
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
    std::shared_ptr<Session> session, Controller& controller, LayoutFn layout)
{
    return ftxui::Make<ChatImpl>(
        std::move(session), controller, std::move(layout));
}

} // namespace ursa
