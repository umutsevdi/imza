#include "app/flows.h"
#include "conversation/persistence.h"
#include "runtime/main_thread_queue.h"
#include "tools/skills.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <print>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace imza {

namespace {

    using namespace ftxui;

    std::string terminal_notification_sequence(
        bool osc9_supported, std::string_view message)
    {
        if (!osc9_supported) {
            return "\a";
        }
        return "\x1B]9;" + std::string(message) + "\a";
    }

    void print_session_saved_box(const SessionStore& store)
    {
        const int term_w = Terminal::Size().dimx;
        const int width  = std::max(40, std::min(term_w, 80));

        std::vector<SavedSession> sessions = store.sessions();
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
                const std::string title = session.title.empty()
                    ? "Untitled session"
                    : session.title;
                rows.push_back(hbox(
                    { text(fit(title, title_col)) | color(PANEL_FG) | xflex,
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

        auto screen = Screen::Create(
            Dimension::Fixed(body_w), Dimension::Fixed(height));
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

    bool terminal_supports_osc9()
    {
        const char* term_program = std::getenv("TERM_PROGRAM");
        if (term_program != nullptr) {
            const std::string_view program(term_program);
            if (program == "iTerm.app" || program == "WezTerm"
                || program == "ghostty") {
                return true;
            }
        }
        const char* windows_terminal = std::getenv("WT_SESSION");
        return windows_terminal != nullptr && *windows_terminal != '\0';
    }

    bool is_reverse_tab(const Event& event)
    {
        return event == Event::TabReverse
            || event == Event::Special("\x1B[9;2u")
            || event == Event::Special("\x1B[27;2;9~");
    }

    void set_bracketed_paste(bool enabled)
    {
        std::cout << (enabled ? "\x1B[?2004h" : "\x1B[?2004l") << std::flush;
    }

    class BracketedPasteMode {
    public:
        explicit BracketedPasteMode(ScreenInteractive& screen)
        {
            screen.Post([] { set_bracketed_paste(true); });
        }

        ~BracketedPasteMode() { disable(); }

        void disable()
        {
            if (!_enabled) {
                return;
            }
            _enabled = false;
            set_bracketed_paste(false);
        }

    private:
        bool _enabled = true;
    };

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
            sidechat_    = make_sidechat_component(
                state_, [this] { _focus_main(); }, sidechat_status_);

            workspace_subscription_
                = state_->environment->subscribe_to_workspace_change(
                    [] { animation::RequestAnimationFrame(); });
            title_subscription_ = state_->session->subscribe_to_title_change(
                [] { animation::RequestAnimationFrame(); });
            phase_            = state_->session->mode() == Session::Mode::PLAN
                ? WorkflowPhase::PLAN
                : WorkflowPhase::BUILD;
            selected_         = static_cast<int>(phase_);
            review_available_ = _review_available();
            tab_names_        = { "Plan", "Build" };
            if (review_available_) {
                tab_names_.emplace_back("Review");
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
            Add(sidechat_);
            chat_->TakeFocus();
        }

        Element OnRender() override
        {
            _sync_review_availability();
            const auto terminal_size = ftxui::Terminal::Size();
            layout_ = layout_context(terminal_size.dimx, terminal_size.dimy);
            const int w       = layout_.width;
            Element side      = side_->Render();
            Element tab       = tabs_->Render();
            Element right_col = tabs_content_->Render();
            Element status    = status_line_->Render();

            const std::string title = state_->session->title();
            Element title_p = paragraph(title.empty() ? "New Session" : title)
                | bold | color(PANEL_FG);
            const bool narrow       = layout_.kind == LayoutCtx::Kind::NARROW;
            const bool side_by_side = state_->sidechat_open && !narrow;

            Element root;
            if (narrow && sidechat_status_.focused) {
                root = vbox({ sidechat_->Render() | flex, separatorEmpty(),
                           status })
                    | flex;
            } else if (layout_.kind == LayoutCtx::Kind::WIDE) {
                Element main_panel
                    = vbox({
                          hbox({ text(" "), title_p | xflex, tab }),
                          std::move(right_col) | reflect(main_pane_box_)
                              | yflex,
                      })
                    | xflex | yflex;
                if (side_by_side) {
                    main_panel = hbox({ std::move(main_panel), separatorEmpty(),
                        sidechat_->Render() });
                }
                root = vbox({ hbox({ side | yflex, text(" "),
                                  std::move(main_panel) | xflex | yflex })
                               | flex,
                           separatorEmpty(), status })
                    | flex;
            } else {
                Element content = std::move(right_col) | xflex | yflex;
                root            = vbox({ hbox({ title_p | xflex, tab }), side,
                                      separatorEmpty(), std::move(content),
                                      separatorEmpty(), status })
                    | flex;
            }

            Component popup_source = nullptr;
            if (state_->session->modal().index() != 0) {
                popup_source = modal_;
            } else if (sidechat_status_.has_modal
                && sidechat_status_.has_modal()) {
                popup_source = sidechat_status_.modal;
            }
            if (popup_source) {
                const int h   = terminal_size.dimy;
                const int mw  = std::min(w - 4, MODAL_MAX_WIDTH);
                const int mh  = std::max(10, h - 4);
                Element popup = popup_source->Render()
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
                state_->on_exit();
                return true;
            }
            if (state_->session->modal().index() != 0) {
                return modal_->OnEvent(event);
            }
            if (sidechat_status_.has_modal && sidechat_status_.has_modal()) {
                return sidechat_->OnEvent(event);
            }
            if (sidechat_->OnEvent(event)) {
                return true;
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
                const Mouse& m = event.mouse();
                if (m.button == Mouse::Left && m.motion == Mouse::Pressed
                    && state_->sidechat_open && sidechat_status_.focused
                    && main_pane_box_.Contain(m.x, m.y)) {
                    _focus_main();
                }
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

        Component ActiveChild() override
        {
            if (state_->sidechat_open && sidechat_status_.focused) {
                return sidechat_;
            }
            return ComponentBase::ActiveChild();
        }

    private:
        bool _review_available() const
        {
            const auto& environment = state_->environment;
            return environment->ready() && environment->system()->has_git
                && environment->workspace()->project_root.has_value();
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
            if (sidechat_status_.focused) {
                return;
            }
            if (selected_pane_ == 0) {
                chat_->TakeFocus();
            } else {
                review_->TakeFocus();
            }
        }

        void _focus_main()
        {
            if (selected_pane_ == 0) {
                chat_->TakeFocus();
            } else {
                review_->TakeFocus();
            }
        }

        ScreenInteractive& screen_;
        std::shared_ptr<ApplicationState> state_;
        ftxui::Component sidechat_;
        SidechatStatus sidechat_status_;
        Component side_;
        Component modal_;
        Component status_line_;
        Component chat_;
        Component review_;
        Component tabs_content_;
        Component tabs_;
        Signal<>::Subscription workspace_subscription_;
        Signal<>::Subscription title_subscription_;
        LayoutCtx layout_ = layout_context(0);
        WorkflowPhase phase_ { WorkflowPhase::PLAN };
        std::vector<std::string> tab_names_;
        bool review_available_ { false };
        int selected_ { 0 };
        int selected_pane_ { 0 };
        ftxui::Box main_pane_box_ { };
    };

} // namespace

int run_repl(
    std::shared_ptr<ApplicationState> state, MainThreadQueue& main_thread)
{
    if (!is_interactive_terminal()) {
        std::println("imza requires an interactive terminal");
        return 1;
    }

    ScreenInteractive screen = ScreenInteractive::FullscreenAlternateScreen();
    screen.ForceHandleCtrlC(false);
    state->notify_user = [](AgentNotification notification) {
        const std::string_view message
            = notification == AgentNotification::TURN_FINISHED
            ? "Imza finished"
            : "Imza needs your attention";
        std::cout << terminal_notification_sequence(
            terminal_supports_osc9(), message)
                  << std::flush;
    };
    auto task_subscription = main_thread.subscribe([&screen, &main_thread] {
        screen.Post([&main_thread] { main_thread.drain(); });
        screen.PostEvent(Event::Custom);
    });
    BracketedPasteMode bracketed_paste(screen);
    state->on_exit = [&screen, &bracketed_paste] {
        bracketed_paste.disable();
        screen.Exit();
    };
    state->providers->ensure_catalog_fresh();
    state->environment->check_for_updates(IMZA_VERSION);
    if (state->providers->config().providers.empty()) {
        imza::enqueue_user_modal(
            *state, ConnectModal { ConnectModal::Entry::MANAGE });
    }
    auto app = ftxui::Make<Repl>(screen, state);
    screen.Loop(app);
    bracketed_paste.disable();
    if (!state->session->has_items()) {
        return 0;
    }
    const bool saved = state->sessions->save(*state->session) == Status::OK;
    if (saved) {
        print_session_saved_box(*state->sessions);
    }
    return saved ? 0 : 1;
}

} // namespace imza
