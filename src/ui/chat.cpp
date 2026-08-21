#include "ui.hpp"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "commands.hpp"
#include "render.hpp"

namespace ursa {

namespace {

using namespace ftxui;

    std::string to_lower(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    }

    Element status_element(const UiState& st)
    {
        if (!st.error.empty()) {
            return hbox({
                text("! ") | bold | color(Color::RedLight),
                text(st.error) | color(Color::RedLight),
            });
        }
        return text("");
    }

    class ChatImpl : public ComponentBase {
    public:
        ChatImpl(Controller& controller, std::function<int()> width)
            : controller_(controller)
            , width_(std::move(width))
        {
            input_options_.content     = &input_buf_;
            input_options_.placeholder = "ask anything — type / for commands";
            input_options_.multiline   = false;
            input_options_.on_change   = [this] { refresh_suggestions(); };
            input_options_.on_enter    = [this] { submit(); };
            input_options_.cursor_position = Ref<int>(&input_cursor_);
            input_                     = ftxui::Input(input_options_);
            Add(input_);
            input_->TakeFocus();
        }

        Element OnRender() override
        {
            const UiState& st = controller_.state();
            LayoutCtx ctx { width_() >= 100 ? LayoutCtx::Kind::WIDE : LayoutCtx::Kind::NARROW,
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

            Element content
                = items.empty() ? text("") : vbox(std::move(items)) | flex;
            Element log = std::move(content) | vscroll_indicator
                | focusPositionRelative(0.0F, scroll_y_) | yframe;

            Element input_box = vbox({
                separatorEmpty(),
                hbox({
                    text("  "),
                    input_->Render() | flex,
                    status_element(st),
                    text("  "),
                }),
                separatorEmpty(),
            }) | bgcolor(PANEL_COLOR);

            Element main = vbox({
                std::move(log) | flex,
            }) | flex;

            Elements bottom;
            if (st.question) {
                bottom.push_back(render_question(*st.question) | borderRounded);
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
            if (show_suggestions()) {
                if (event == Event::ArrowDown) {
                    sel_ = (sel_ + 1) % static_cast<int>(matches_.size());
                    return true;
                }
                if (event == Event::ArrowUp) {
                    const int n = static_cast<int>(matches_.size());
                    sel_ = (sel_ - 1 + n) % n;
                    return true;
                }
                if (event == Event::Tab || event == Event::Return) {
                    accept();
                    return true;
                }
                if (event == Event::Escape) {
                    matches_.clear();
                    return true;
                }
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
            if (event == Event::ArrowUp) {
                scroll_by(-0.05F);
                return true;
            }
            if (event == Event::ArrowDown) {
                scroll_by(0.05F);
                return true;
            }
            if (event == Event::PageUp) {
                scroll_by(-0.35F);
                return true;
            }
            if (event == Event::PageDown) {
                scroll_by(0.35F);
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
            sel_ = 0;
            const std::string text = input_buf_;
            if (text.empty() || text[0] != '/' || text.find(' ') != std::string::npos) {
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
            input_buf_ = cmd->name;
            input_cursor_ = static_cast<int>(input_buf_.size());
            refresh_suggestions();
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
                const bool sel = static_cast<int>(i) == sel_;
                Element name = text(c.name);
                if (sel) {
                    name = name | bold;
                }
                Element row = hbox({
                    name,
                    text("   "),
                    text(c.desc) | dim | color(Color::GrayLight),
                });
                row = row | (sel ? bgcolor(Color::Blue) : bgcolor(PANEL_COLOR));
                rows.push_back(std::move(row));
            }
            if (total > shown) {
                rows.push_back(text("  … " + std::to_string(total - shown) + " more")
                    | dim | color(Color::GrayLight));
            }
            return vbox(std::move(rows)) | borderRounded | bgcolor(PANEL_COLOR);
        }

        Controller& controller_;
        std::function<int()> width_;

        std::string input_buf_;
        InputOption input_options_;
        Component input_;

        std::vector<const SlashCommand*> matches_;
        int sel_ = 0;
        int input_cursor_ = 0;

        float scroll_y_ = 1.0F;
        int frame_      = 0;
    };

} // namespace

ftxui::Component make_chat(Controller& controller, std::function<int()> width)
{
    return ftxui::Make<ChatImpl>(controller, std::move(width));
}

} // namespace ursa
