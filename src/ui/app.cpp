#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <functional>
#include <print>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ursa {

namespace {

    using namespace ftxui;

    bool is_interactive_terminal()
    {
#ifdef _WIN32
        return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
        return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
    }

    class Repl : public ComponentBase {
    public:
        Repl(ScreenInteractive& screen, std::shared_ptr<Session> session,
            Controller& controller)
            : screen_(screen)
            , session_(std::move(session))
            , controller_(controller)
        {
            const LayoutFn layout = [this] { return layout_; };
            side_        = make_side_panel(session_, controller, layout);
            chat_        = make_chat(session_, controller, layout);
            status_line_ = make_status_line(session_, controller, layout);
            modal_       = make_modal(session_, controller);
            Add(chat_);
        }

        Element OnRender() override
        {
            const auto terminal_size = ftxui::Terminal::Size();
            layout_                  = layout_context(terminal_size.dimx);
            const int w              = layout_.width;

            Element side      = side_->Render();
            Element right_col = chat_->Render();
            Element status    = status_line_->Render();

            const int chat_w = (layout_.kind == LayoutCtx::Kind::WIDE)
                ? w - LayoutCtx::panel_width - 1
                : w;
            right_col
                = std::move(right_col) | size(WIDTH, EQUAL, chat_w) | yflex;

            Element root;
            if (layout_.kind == LayoutCtx::Kind::WIDE) {
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

            if (session_->modal().index() != 0) {
                const int h   = terminal_size.dimy;
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
            if (session_->modal().index() != 0) {
                return modal_->OnEvent(event);
            }
            return chat_->OnEvent(event);
        }

    private:
        ScreenInteractive& screen_;
        std::shared_ptr<Session> session_;
        Controller& controller_;
        Component side_;
        Component chat_;
        Component modal_;
        Component status_line_;
        LayoutCtx layout_ = layout_context(0);
    };

} // namespace

int run_repl(const Config& cfg)
{
    if (!is_interactive_terminal()) {
        std::println("ursa requires an interactive terminal");
        return 1;
    }

    ScreenInteractive screen = ScreenInteractive::FullscreenAlternateScreen();
    ToolRegistry tools       = builtin_tools();
    auto session             = std::make_shared<Session>();
    Controller controller(
        session, cfg,
        [&screen](std::function<void()> f) {
            screen.Post(std::move(f));
            screen.PostEvent(Event::Custom);
        },
        [&screen] { screen.Exit(); }, StreamFn { }, std::move(tools));
    controller.ensure_catalog_fresh();
    if (controller.config().providers.empty()) {
        controller.enqueue_user_modal(
            ConnectModal { ConnectModal::Entry::MANAGE });
    }
    auto app = ftxui::Make<Repl>(screen, session, controller);
    screen.Loop(app);
    return 0;
}

} // namespace ursa
