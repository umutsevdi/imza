#include "ui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <unistd.h>

#include <functional>

#include "render.hpp"
#include <print>

namespace ursa {

namespace {

    using namespace ftxui;

    Element header_element(const Config& cfg)
    {
        const std::string cwd = std::filesystem::current_path().string();
        Element bar = hbox({
            text(" "),
            text("ursa") | bold | color(Color::White),
            text(cfg.model.empty() ? "" : "  ·  " + cfg.model)
                | color(Color::GrayLight),
            filler(),
            text(cwd) | color(Color::GrayLight),
            text(" "),
        });
        return std::move(bar) | bgcolor(Color::GrayDark);
    }

    class Repl : public ComponentBase {
    public:
        Repl(ScreenInteractive& screen, Controller& controller)
            : screen_(screen)
            , controller_(controller)
        {
            todo_ = make_todo(controller, [] { return 30; });
            files_ = make_changed_files(controller);
            chat_  = make_chat(controller,
                [&screen] { screen.Exit(); },
                [] { return ftxui::Terminal::Size().dimx; });
            Add(chat_);
        }

        Element OnRender() override
        {
            const int w = ftxui::Terminal::Size().dimx;
            const LayoutCtx::Kind kind = w >= 100 ? LayoutCtx::Kind::WIDE : LayoutCtx::Kind::NARROW;

            Element side
                = vbox({ todo_->Render() | yflex, files_->Render() | yflex })
                | bgcolor(PANEL_COLOR);

            Element right_col = chat_->Render();
            const int chat_w   = (kind == LayoutCtx::Kind::WIDE) ? w - 31 : w;
            right_col          = std::move(right_col) | size(WIDTH, EQUAL, chat_w);
            Element header    = header_element(controller_.config());

            if (kind == LayoutCtx::Kind::WIDE) {
                return vbox({ header,
                    separatorEmpty(),
                    hbox({ text(" "), side | size(WIDTH, EQUAL, 30) | yflex, text(" "), right_col })
                        | flex })
                    | flex;
            }
            return vbox({ header, separatorEmpty(), side, right_col }) | flex;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::CtrlC || event == Event::CtrlD) {
                screen_.Exit();
                return true;
            }
            return chat_->OnEvent(event);
        }

    private:
        ScreenInteractive& screen_;
        Controller& controller_;
        Component todo_;
        Component files_;
        Component chat_;
    };

} // namespace

int run_repl(const Config& cfg)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::println("ursa requires an interactive terminal");
        return 1;
    }

    ScreenInteractive screen = ScreenInteractive::FullscreenAlternateScreen();
    Controller controller(cfg, [&screen](std::function<void()> f) { screen.Post(f); });
    auto app = ftxui::Make<Repl>(screen, controller);
    screen.Loop(app);
    return 0;
}

} // namespace ursa
