#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <unistd.h>
#include <algorithm>
#include <functional>

#include "environment.h"
#include "ui.h"
#include <print>

namespace ursa {

namespace {

    using namespace ftxui;

    class Repl : public ComponentBase {
    public:
        Repl(ScreenInteractive& screen, Controller& controller)
            : screen_(screen)
            , controller_(controller)
        {
            side_ = make_side_panel(
                controller, [] { return ftxui::Terminal::Size().dimx; });
            chat_ = make_chat(
                controller, [] { return ftxui::Terminal::Size().dimx; });
            status_line_ = make_status_line(
                controller, [] { return ftxui::Terminal::Size().dimx; });
            ;
            modal_ = make_modal(controller);
            Add(chat_);
        }

        Element OnRender() override
        {
            const int w                = ftxui::Terminal::Size().dimx;
            const LayoutCtx::Kind kind = w >= LayoutCtx::wide_threshold
                ? LayoutCtx::Kind::WIDE
                : LayoutCtx::Kind::NARROW;

            Element side      = side_->Render();
            Element right_col = chat_->Render();
            Element status    = status_line_->Render();

            const int chat_w = (kind == LayoutCtx::Kind::WIDE)
                ? w - LayoutCtx::panel_width - 1
                : w;
            right_col
                = std::move(right_col) | size(WIDTH, EQUAL, chat_w) | yflex;

            Element root;
            if (kind == LayoutCtx::Kind::WIDE) {
                root = vbox({ separatorEmpty(),
                           hbox({ text(" "), side | yflex, text(" "),
                               right_col })
                               | flex,
                           separatorEmpty(), status })
                    | flex;
            } else {
                root = vbox({ separatorEmpty(), side, right_col,
                           separatorEmpty(), status })
                    | flex;
            }

            if (controller_.state().modal.index() != 0) {
                const int h   = ftxui::Terminal::Size().dimy;
                const int mw  = std::min(w - 4, MODAL_MAX_WIDTH);
                const int mh  = std::max(10, h - 4);
                Element popup = modal_->Render()
                    | borderStyled(ROUNDED, PANEL_BORDER) | bgcolor(PANEL_COLOR)
                    | color(PANEL_FG) | clear_under | size(WIDTH, EQUAL, mw)
                    | size(HEIGHT, LESS_THAN, mh);
                root = dbox({ dim(std::move(root)), center(std::move(popup)) });
            }
            return root;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::CtrlC || event == Event::CtrlD) {
                screen_.Exit();
                return true;
            }
            if (controller_.state().modal.index() != 0) {
                return modal_->OnEvent(event);
            }
            return chat_->OnEvent(event);
        }

    private:
        ScreenInteractive& screen_;
        Controller& controller_;
        Component side_;
        Component chat_;
        Component modal_;
        Component status_line_;
    };

} // namespace

int run_repl(const Config& cfg)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::println("ursa requires an interactive terminal");
        return 1;
    }

    ScreenInteractive screen = ScreenInteractive::FullscreenAlternateScreen();
    ToolRegistry tools       = builtin_tools();
    auto env                 = analyze_environment_async();
    Controller controller(
        cfg,
        [&screen](std::function<void()> f) {
            screen.Post(std::move(f));
            screen.PostEvent(Event::Custom);
        },
        [&screen] { screen.Exit(); }, StreamFn { }, std::move(tools),
        std::move(env));
    controller.ensure_catalog_fresh();
    if (controller.config().providers.empty()) {
        controller.enqueue_user_modal(
            ConnectModal { ConnectModal::Entry::MANAGE });
    }
    auto app = ftxui::Make<Repl>(screen, controller);
    screen.Loop(app);
    return 0;
}

} // namespace ursa
