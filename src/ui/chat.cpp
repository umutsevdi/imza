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

#include "render.hpp"

namespace ursa {

namespace {

    using namespace ftxui;

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
        ChatImpl(Controller& controller, std::function<void()> on_exit,
            std::function<int()> width)
            : controller_(controller)
            , on_exit_(std::move(on_exit))
            , width_(std::move(width))
        {
            input_options_.content     = &input_buf_;
            input_options_.placeholder = "ask anything — /exit quits";
            input_options_.multiline   = false;
            input_options_.on_enter    = [this] { submit(); };
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
            if (text == "/exit" || text == "/quit") {
                on_exit_();
                return;
            }
            controller_.submit(std::move(text));
            scroll_y_ = 1.0F;
            animation::RequestAnimationFrame();
        }

        Controller& controller_;
        std::function<void()> on_exit_;
        std::function<int()> width_;

        std::string input_buf_;
        InputOption input_options_;
        Component input_;

        float scroll_y_ = 1.0F;
        int frame_      = 0;
    };

} // namespace

ftxui::Component make_chat(
    Controller& controller, std::function<void()> on_exit, std::function<int()> width)
{
    return ftxui::Make<ChatImpl>(controller, std::move(on_exit), std::move(width));
}

} // namespace ursa
