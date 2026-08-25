#include "ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "format.h"
#include "util.h"

namespace ursa {

using namespace ftxui;

namespace {

    using namespace ftxui;

    Element error_element(const UiState& st)
    {
        if (st.error.empty()) {
            return text("");
        }
        return hbox({
            text(" ") | bgcolor(PANEL_COLOR),
            text(" ") | bgcolor(Color::Red),
            text(" " + st.error) | bgcolor(Color::Red),
            filler() | bgcolor(Color::Red),
        });
    }

    Element user_item(const UserTurn& t)
    {
        return card(render_markdown_element(t.text), PANEL_COLOR);
    }

    Element assistant_item(const AssistantTurn& t)
    {
        return card(render_markdown_element(t.markdown));
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
                    if (tc.name == "read") {
                        rows.push_back(code_block_with_lines(tc.result->text,
                            tool_code_language(tc), read_start_line(tc)));
                    } else if (tc.name == "list") {
                        rows.push_back(list_block(tc.result->text));
                    } else if (tc.name == "ask") {
                        rows.push_back(render_markdown_element(tc.result->text));
                    } else {
                        rows.push_back(
                            code_block(tc.result->text, tool_code_language(tc)));
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
        return card(vbox(std::move(rows)));
    }

    Element modal_answer_item(const ModalAnswer& ans)
    {
        return card(
            render_markdown_element(modal_answer_markdown(ans)), PANEL_COLOR);
    }

    class ChatImpl : public ComponentBase {
    public:
        ChatImpl(Controller& controller, std::function<int()> width)
            : controller_(controller)
            , width_(std::move(width))
        {
            input_options_.content     = &input_buf_;
            input_options_.placeholder = "ask anything — type / for commands";
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
            input_ = ftxui::Input(input_options_);
            Add(input_);
            input_->TakeFocus();
        }

        Element OnRender() override
        {
            const UiState& st = controller_.state();
            LayoutCtx ctx { width_() >= 100 ? LayoutCtx::Kind::WIDE
                                            : LayoutCtx::Kind::NARROW,
                width_() };

            const bool streaming = st.phase == UiState::Phase::STREAMING;

            Elements items;
            for (const auto& it : st.items) {
                if (!items.empty()) {
                    items.push_back(text(""));
                }
                Element el = render_item(it, ctx);
                if (streaming && std::holds_alternative<AssistantTurn>(it)
                    && &it == &st.items.back()) {
                    el = vbox({
                        hbox({
                            spinner(15, static_cast<size_t>(frame_))
                                | color(Color::GrayLight),
                            text(" thinking…") | dim,
                        }),
                        el,
                    });
                }
                items.push_back(std::move(el));
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

            Element content
                = items.empty() ? text("") : vbox(std::move(items)) | flex;
            Element log = std::move(content) | vscroll_indicator
                | focusPositionRelative(0.0F, scroll_y_) | yframe;

            Element input_box = panel(vbox({
                separatorEmpty(),
                hbox({
                    text("  "),
                    input_->Render() | xflex,
                    text("  "),
                }),
                hbox({
                    text("  "),
                    text("Tab: switch mode, Alt+Enter: multi line input")
                        | color(PANEL_FG_DIM),
                }),
                separatorEmpty(),
            }));

            Element main = vbox({
                               std::move(log) | flex,
                           })
                | flex;

            Elements bottom;
            if (!st.error.empty()) {
                bottom.push_back(error_element(st));
            }
            if (show_suggestions()) {
                bottom.push_back(render_suggestions());
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
                    scroll_by(-0.04F);
                    return true;
                }
                if (m.button == Mouse::WheelDown) {
                    scroll_by(0.04F);
                    return true;
                }
                return false;
            }
            const bool multiline_input
                = input_buf_.find('\n') != std::string::npos;
            if (!multiline_input) {
                if (event == Event::ArrowUp) {
                    scroll_by(-0.05F);
                    return true;
                }
                if (event == Event::ArrowDown) {
                    scroll_by(0.05F);
                    return true;
                }
            }
            if (event == Event::PageUp) {
                scroll_by(-0.35F);
                return true;
            }
            if (event == Event::PageDown) {
                scroll_by(0.35F);
                return true;
            }
            if (event == Event::Return) {
                if (paste_mode_) {
                    insert_newline();
                } else {
                    submit();
                }
                return true;
            }
            return input_->OnEvent(event);
        }

        void OnAnimation(animation::Params&) override
        {
            if (controller_.state().phase != UiState::Phase::STREAMING) {
                return;
            }
            ++frame_;
            animation::RequestAnimationFrame();
        }

    private:
        void scroll_by(float delta)
        {
            scroll_y_ = std::clamp(scroll_y_ + delta, 0.0F, 1.0F);
        }

        void on_input_changed()
        {
            controller_.state().error.clear();
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
            scroll_y_ = 1.0F;
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

        std::string input_buf_;
        InputOption input_options_;
        Component input_;

        std::vector<const SlashCommand*> matches_;
        int sel_          = 0;
        int input_cursor_ = 0;
        bool paste_mode_  = false;

        float scroll_y_ = 1.0F;
        int frame_      = 0;
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
