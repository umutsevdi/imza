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
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "format.h"
#include "util.h"

namespace ursa {

using namespace ftxui;

namespace {

    using namespace ftxui;

    constexpr std::size_t kLargeOutputLines = 30;
    constexpr std::size_t kInvalidVersion   = ~std::size_t { 0 };
    constexpr int kWheelStep                = 3;

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

    Element error_element(const UiState& st)
    {
        std::string msg = st.error;
        if (st.retry_countdown) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                st.retry_countdown->deadline - std::chrono::steady_clock::now())
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
            text(" ") | bgcolor(PANEL_COLOR),
            text(" ") | bgcolor(Color::Red),
            text(" " + msg) | bgcolor(Color::Red),
            filler() | bgcolor(Color::Red),
        });
    }

    std::size_t item_version(const ConversationItem& it)
    {
        if (const auto* a = std::get_if<AssistantTurn>(&it)) {
            return a->markdown.size();
        }
        if (const auto* tc = std::get_if<ToolCall>(&it)) {
            if (!tc->result.has_value()) {
                return 0;
            }
            return 1 + tc->result->text.size();
        }
        return 0;
    }

    Element user_item(const UserTurn& t)
    {
        return card(render_markdown_element(t.text), PANEL_COLOR, false);
    }

    Element assistant_item(const AssistantTurn& t)
    {
        return card(render_markdown_element(t.markdown), std::nullopt, false);
    }

    Element toolcall_item(const ToolCall& tc)
    {
        Elements rows;
        rows.push_back(hbox({
            text("Tool Call: ") | bold | color(Color::GreenLight),
            text(tool_call_head(tc)) | color(PANEL_FG),
        }));
        if (tc.result.has_value()) {
            switch (tc.result->kind) {
            case ToolCall::Result::Kind::OUTPUT:
                if (!tc.result->text.empty()) {
                    if (tc.name == "list") {
                        rows.push_back(list_block(tc.result->text));
                    } else if (tc.name == "ask") {
                        rows.push_back(
                            render_markdown_element(tc.result->text));
                    } else {
                        rows.push_back(code_block(
                            tc.result->text, tool_code_language(tc)));
                    }
                }
                break;
            case ToolCall::Result::Kind::ERROR:
                rows.push_back(hbox({
                    text("Error: ") | bold | color(Color::RedLight),
                    text(tc.result->text) | color(Color::RedLight),
                }));
                break;
            case ToolCall::Result::Kind::REJECT:
                rows.push_back(hbox({
                    text("Rejected: ") | bold | color(Color::YellowLight),
                    text(tc.result->text) | color(Color::YellowLight),
                }));
                break;
            case ToolCall::Result::Kind::CANCEL: break;
            }
        }
        return card(vbox(std::move(rows)), std::nullopt, false);
    }

    Element modal_answer_item(const ModalAnswer& ans)
    {
        return card(render_markdown_element(modal_answer_markdown(ans)),
            PANEL_COLOR, false);
    }

    class ChatImpl : public ComponentBase {
    public:
        ChatImpl(Controller& controller, std::function<int()> width)
            : controller_(controller)
            , width_(std::move(width))
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
                state.element |= bgcolor(PANEL_COLOR);
                return state.element;
            };
            input_     = ftxui::Input(input_options_);
            container_ = Container::Vertical({ input_ });
            Add(container_);
            input_->TakeFocus();
        }

        Element OnRender() override
        {
            const UiState& st = controller_.state();
            LayoutCtx ctx { width_() >= LayoutCtx::wide_threshold
                    ? LayoutCtx::Kind::WIDE
                    : LayoutCtx::Kind::NARROW,
                width_() };

            const bool streaming  = st.phase == UiState::Phase::STREAMING;
            const bool connecting = st.phase == UiState::Phase::CONNECTING;

            Elements items;
            if (follow_) {
                scroll_top_ = max_scroll();
            } else {
                scroll_top_ = std::clamp(scroll_top_, 0, max_scroll());
            }
            if (cache_kind_ != ctx.kind
                || item_cache_.size() != st.items.size()) {
                item_cache_.clear();
                item_cache_.resize(st.items.size());
                item_versions_.assign(st.items.size(), kInvalidVersion);
                cache_kind_ = ctx.kind;
            }
            std::size_t item_index = 0;
            for (const auto& it : st.items) {
                const std::size_t version = item_version(it);
                if (item_versions_[item_index] != version) {
                    if (std::holds_alternative<ToolCall>(it)) {
                        const ToolCall& tc = std::get<ToolCall>(it);
                        const bool output  = tc.result.has_value()
                            && tc.result->kind == ToolCall::Result::Kind::OUTPUT
                            && !tc.result->text.empty();
                        if (tc.name == "read" && output) {
                            item_cache_[item_index] = render_read_item(tc);
                        } else if (tc.name == "shell" && output
                            && count_lines(tc.result->text)
                                > kLargeOutputLines) {
                            item_cache_[item_index]
                                = render_shell_collapsed(tc);
                        } else {
                            item_cache_[item_index] = render_item(it, ctx);
                        }
                    } else {
                        item_cache_[item_index] = render_item(it, ctx);
                    }
                    item_versions_[item_index] = version;
                }
                Element el = item_cache_[item_index];
                if ((streaming || connecting)
                    && std::holds_alternative<AssistantTurn>(it)
                    && &it == &st.items.back()) {
                    el = vbox({
                        hbox({
                            spinner(15, static_cast<size_t>(frame_))
                                | color(Color::GrayLight),
                            text(connecting ? " connecting…" : " thinking…")
                                | dim,
                        }),
                        el,
                    });
                }
                items.push_back(hbox({
                    text(" "),
                    std::move(el) | xflex,
                }));
                ++item_index;
            }

            const size_t queued_n = st.queued.size();
            for (size_t i = 0; i < queued_n; ++i) {
                const auto& q = st.queued[i];
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
            Element main = vbox({
                               std::move(log) | flex,
                           })
                | flex | reflect(frame_box_);

            Elements bottom;
            if (show_suggestions()) {
                bottom.push_back(render_suggestions());
            }
            bottom.push_back(
                hbox({
                    filler(),
                    text("↑/↓ scroll · click a card to open in viewer")
                        | color(PANEL_FG_DIM),
                    text(" "),
                })
                | xflex);
            bottom.push_back(
                hbox({
                    filler(),
                    text("Tab: switch mode, Alt+Enter: multi line input")
                        | color(PANEL_FG_DIM),
                    text(" "),
                })
                | xflex);
            if (!st.error.empty() || st.retry_countdown) {
                bottom.push_back(error_element(st));
            }
            bottom.push_back(std::move(input_box));
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
            }
            if (event == Event::Escape) {
                if (show_suggestions()) {
                    matches_.clear();
                    return true;
                }
                if (!controller_.state().queued.empty()) {
                    controller_.cancel_queued(
                        controller_.state().queued.back().id);
                    return true;
                }
                return true;
            }
            if (show_suggestions()) {
                if (event == Event::ArrowDown) {
                    sel_ = (sel_ + 1) % static_cast<int>(matches_.size());
                    return true;
                }
                if (event == Event::ArrowUp) {
                    const int n = static_cast<int>(matches_.size());
                    sel_        = (sel_ - 1 + n) % n;
                    return true;
                }
                if (event == Event::Tab) {
                    accept();
                    return true;
                }
                if (event == Event::Return) {
                    execute_selected();
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
            const auto phase = controller_.state().phase;
            if (phase != UiState::Phase::STREAMING
                && phase != UiState::Phase::CONNECTING) {
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
            } else {
                controller_.enqueue_user_modal(
                    ViewerModal { "Shell output", tc.result->text, "", 1 });
            }
        }

        void on_input_changed()
        {
            controller_.clear_error();
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
            controller_.submit(std::move(text));
            follow_ = true;
            animation::RequestAnimationFrame();
        }

        void refresh_suggestions()
        {
            matches_.clear();
            sel_                   = 0;
            const std::string text = input_buf_;
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
            const SlashCommand* cmd = matches_[sel_];
            input_buf_              = cmd->name;
            input_cursor_           = static_cast<int>(input_buf_.size());
            refresh_suggestions();
        }

        void execute_selected()
        {
            const SlashCommand* cmd = matches_[sel_];
            input_buf_              = cmd->name;
            input_cursor_           = static_cast<int>(input_buf_.size());
            matches_.clear();
            sel_ = 0;
            submit();
        }

        bool show_suggestions() const { return !matches_.empty(); }

        Element render_suggestions()
        {
            const size_t max_rows = 8;
            const size_t total    = matches_.size();
            const size_t shown    = std::min(total, max_rows);
            Elements rows;
            for (size_t i = 0; i < shown; ++i) {
                const SlashCommand& c = *matches_[i];
                const bool sel        = static_cast<int>(i) == sel_;
                Element name          = text(c.name);
                if (sel) {
                    name = name | bold;
                }
                Element row = hbox({
                    name,
                    text("   "),
                    text(c.desc) | dim | color(PANEL_FG_DIM),
                });
                row = row | (sel ? bgcolor(Color::Blue) : bgcolor(PANEL_COLOR));
                rows.push_back(std::move(row));
            }
            if (total > shown) {
                rows.push_back(
                    text("  … " + std::to_string(total - shown) + " more") | dim
                    | color(PANEL_FG_DIM));
            }
            return vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER)
                | bgcolor(PANEL_COLOR) | color(PANEL_FG);
        }

        Controller& controller_;
        std::function<int()> width_;

        Component container_;
        std::map<std::size_t, Component> read_buttons_;

        std::vector<Element> item_cache_;
        std::vector<std::size_t> item_versions_;
        LayoutCtx::Kind cache_kind_ = LayoutCtx::Kind::NARROW;

        Element render_read_item(const ToolCall& tc)
        {
            const std::string head    = tool_call_head(tc);
            const std::string content = tc.result->text;
            const std::string label   = "▸ open " + head + " in viewer ("
                + std::to_string(count_lines(content)) + " lines)";
            Component btn = make_viewer_button(tc.id, label);
            Elements rows;
            rows.push_back(hbox({
                text("Tool Call: ") | bold | color(Color::GreenLight),
                text(head) | color(PANEL_FG),
            }));
            rows.push_back(separatorEmpty());
            rows.push_back(btn->Render());
            rows.push_back(separatorEmpty());
            return card(vbox(std::move(rows)), std::nullopt, false);
        }

        Element render_shell_collapsed(const ToolCall& tc)
        {
            const std::string& full   = tc.result->text;
            const std::size_t total   = count_lines(full);
            const std::string preview = take_lines(full, kLargeOutputLines);
            const std::string label   = "▸ open full shell output in viewer ("
                + std::to_string(total) + " lines)";
            Component btn = make_viewer_button(tc.id, label);
            Elements rows;
            rows.push_back(hbox({
                text("Tool Call: ") | bold | color(Color::GreenLight),
                text("Shell") | color(PANEL_FG),
            }));
            rows.push_back(separatorEmpty());
            rows.push_back(code_block(preview, ""));
            rows.push_back(separatorEmpty());
            rows.push_back(btn->Render());
            rows.push_back(separatorEmpty());
            return card(vbox(std::move(rows)), std::nullopt, false);
        }

        Component make_viewer_button(std::size_t id, std::string label)
        {
            auto it = read_buttons_.find(id);
            if (it != read_buttons_.end()) {
                return it->second;
            }
            const auto on_click = [this, id] {
                for (const auto& item : controller_.state().items) {
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

        std::string input_buf_;
        InputOption input_options_;
        Component input_;

        std::vector<const SlashCommand*> matches_;
        int sel_          = 0;
        int input_cursor_ = 0;
        bool paste_mode_  = false;

        int scroll_top_     = 0;
        bool follow_        = true;
        int frame_          = 0;
        int content_height_ = 0;
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
            } else if constexpr (std::is_same_v<T, ToolCall>) {
                return toolcall_item(v);
            } else if constexpr (std::is_same_v<T, TodoList>) {
                return render_todo(v, ctx);
            } else if constexpr (std::is_same_v<T, ModalAnswer>) {
                return modal_answer_item(v);
            }
            return text("");
        },
        item);
}

ftxui::Component make_chat(Controller& controller, std::function<int()> width)
{
    return ftxui::Make<ChatImpl>(controller, std::move(width));
}

} // namespace ursa
