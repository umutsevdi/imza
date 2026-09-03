#include "agent/flows.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <print>

#include "subsystems/environment.h"
#include "subsystems/review.h"
#include "subsystems/session_store.h"
#include "subsystems/skill_store.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ursa {

namespace {

    using namespace ftxui;

    void print_session_saved_box()
    {
        const int term_w = Terminal::Size().dimx;
        const int width  = std::max(40, std::min(term_w, 80));

        std::vector<SavedSession> sessions = saved_sessions();
        if (static_cast<int>(sessions.size()) > 5) {
            sessions.resize(5);
        }

        const int inner  = width - 2;
        const int body_w = std::max(inner, 48);

        Elements rows;
        if (sessions.empty()) {
            rows.push_back(text("No other saved sessions.") | dim);
        } else {
            rows.push_back(text("Previous Sessions") | bold);
            const int stamp_col = 16;
            const int title_col = std::max(body_w - 2 - stamp_col, 1);
            for (const auto& session : sessions) {
                const std::string title
                    = session.title.empty() ? "Untitled session" : session.title;
                rows.push_back(
                    hbox({ text(fit(title, title_col)) | color(PANEL_FG) | xflex,
                        text(session.saved_at) | color(PANEL_FG_DIM) }));
            }
        }

        const int height = static_cast<int>(rows.size() + 6);
        Element frame    = vbox({
            text("Session has been saved."),
            text("Continue with /session next time you launch.") | dim | italic,
            separatorEmpty(),
            vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER)
                | bgcolor(PANEL_COLOR) | color(PANEL_FG),
        });

        auto screen
            = Screen::Create(Dimension::Fixed(body_w), Dimension::Fixed(height));
        Render(screen, frame);
        std::cout << screen.ToString() << std::endl;
    }

    bool is_interactive_terminal()
    {
#ifdef _WIN32
        return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
        return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
    }

    bool is_reverse_tab(const Event& event)
    {
        return event == Event::TabReverse
            || event == Event::Special("\x1B[9;2u")
            || event == Event::Special("\x1B[27;2;9~");
    }

    class Repl : public ComponentBase {
    public:
        Repl(ScreenInteractive& screen, std::shared_ptr<ApplicationState> state)
            : screen_(screen)
            , state_(std::move(state))
        {
            const LayoutFn layout     = [this] { return layout_; };
            const WorkflowFn workflow = [this] { return phase_; };
            side_        = make_side_panel(state_, layout, workflow,
                [this](WorkflowPhase phase) { _set_phase(phase); });
            status_line_ = make_status_line(state_, layout, workflow);
            chat_        = make_chat(state_, layout);
            review_      = make_review(state_, layout,
                [this](WorkflowPhase phase) { _set_phase(phase); });
            modal_       = make_modal(state_);

            workspace_subscription_
                = state_->environment->subscribe_to_workspace_change(
                    [] { animation::RequestAnimationFrame(); });

            phase_            = state_->session->mode() == Session::Mode::PLAN
                ? WorkflowPhase::PLAN
                : WorkflowPhase::BUILD;
            selected_         = static_cast<int>(phase_);
            review_available_ = _review_available();
            tab_names_        = { "Plan", "Build" };
            if (review_available_) {
                tab_names_.push_back("Review");
            }
            tabs_ = CatchEvent(
                Menu(&tab_names_, &selected_, MenuOption::HorizontalAnimated()),
                [](const Event& event) {
                    return event == Event::Tab || event == Event::TabReverse;
                });
            tabs_content_ = Container::Tab({ chat_, review_ }, &selected_pane_);
            Add(Container::Stacked({
                Container::Vertical({ tabs_, tabs_content_ }),
                side_,
                status_line_,
                modal_,
            }));
            chat_->TakeFocus();
        }

        Element OnRender() override
        {
            _sync_review_availability();
            const auto terminal_size = ftxui::Terminal::Size();
            layout_                  = layout_context(terminal_size.dimx);
            const int w              = layout_.width;
            Element side             = side_->Render();
            Element tab              = tabs_->Render();
            Element right_col        = tabs_content_->Render();
            Element status           = status_line_->Render();

            right_col = std::move(right_col) | xflex | yflex;

            Element root;
            if (layout_.kind == LayoutCtx::Kind::WIDE) {
                root = vbox({ hbox({ text(" "), side | yflex, text(" "),
                                  vbox({ tab, right_col }) | flex })
                               | flex,
                           separatorEmpty(), status })
                    | flex;
            } else {
                root = vbox({ side, tab, separatorEmpty(), right_col,
                           separatorEmpty(), status })
                    | flex;
            }

            if (state_->session->modal().index() != 0) {
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
            if (state_->session->modal().index() != 0) {
                return modal_->OnEvent(event);
            }
            if (event == Event::Tab) {
                _set_phase(next_workflow_phase(phase_, review_available_));
                return true;
            }
            if (is_reverse_tab(event)) {
                _set_phase(previous_workflow_phase(phase_, review_available_));
                return true;
            }
            if (event.is_mouse()) {
                const int previous = selected_;
                if (tabs_->OnEvent(event)) {
                    if (selected_ != previous) {
                        _set_phase(static_cast<WorkflowPhase>(selected_));
                    }
                    return true;
                }
                if (side_->OnEvent(event)) {
                    return true;
                }
            }
            return selected_pane_ == 0 ? chat_->OnEvent(event)
                                       : review_->OnEvent(event);
        }

    private:
        bool _review_available() const
        {
            const auto& environment = state_->environment;
            return environment->ready() && environment->system()->has_git
                && environment->workspace() != nullptr;
        }

        void _sync_review_availability()
        {
            const bool available = _review_available();
            if (available == review_available_) {
                return;
            }
            review_available_ = available;
            tab_names_        = { "Plan", "Build" };
            if (review_available_) {
                tab_names_.push_back("Review");
                return;
            }
            if (phase_ == WorkflowPhase::REVIEW) {
                _set_phase(WorkflowPhase::PLAN);
            }
        }

        void _set_phase(WorkflowPhase phase)
        {
            phase_         = phase;
            selected_      = static_cast<int>(phase);
            selected_pane_ = phase == WorkflowPhase::REVIEW ? 1 : 0;
            if (const auto mode = workflow_mode(phase)) {
                state_->session->set_mode(*mode);
            }
            if (selected_pane_ == 0) {
                chat_->TakeFocus();
            } else {
                review_->TakeFocus();
            }
        }

        ScreenInteractive& screen_;
        std::shared_ptr<ApplicationState> state_;
        Component side_;
        Component modal_;
        Component status_line_;
        Component chat_;
        Component review_;
        Component tabs_content_;
        Component tabs_;
        Signal<>::Subscription workspace_subscription_;
        LayoutCtx layout_ = layout_context(0);
        WorkflowPhase phase_ { WorkflowPhase::PLAN };
        std::vector<std::string> tab_names_;
        bool review_available_ { false };
        int selected_ { 0 };
        int selected_pane_ { 0 };
    };

} // namespace

int run_repl(const Config& cfg)
{
    if (!is_interactive_terminal()) {
        std::println("ursa requires an interactive terminal");
        return 1;
    }

    ScreenInteractive screen = ScreenInteractive::FullscreenAlternateScreen();
    screen.ForceHandleCtrlC(false);
    std::vector<Tool> tools = default_tools();
    auto state = make_application_state(
        [&screen](std::function<void()> f) {
            screen.Post(std::move(f));
            screen.PostEvent(Event::Custom);
        },
        cfg, StreamFn { }, std::move(tools));
    state->on_exit = [&screen] { screen.Exit(); };
    state->providers->ensure_catalog_fresh();
    if (state->providers->config().providers.empty()) {
        ursa::enqueue_user_modal(*state, ConnectModal { ConnectModal::Entry::MANAGE });
    }
    auto app = ftxui::Make<Repl>(screen, state);
    screen.Loop(app);
    if (state->session->snapshot().items.empty()) {
        return 0;
    }
    const bool saved = save_session(*state->session) == Status::OK;
    print_session_saved_box();
    return saved ? 0 : 1;
}

} // namespace ursa
