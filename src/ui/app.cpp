#include "app/flows.h"
#include "conversation/persistence.h"
#include "platform/clipboard.h"
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
                const std::string title
                    = session.title.empty() ? UNTITLED_TITLE : session.title;
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

    // Copies a finished mouse selection to the clipboard. A release at the
    // last motion position writes the same selection coordinates, so FTXUI
    // fires no change callback; the copy then runs on the release itself,
    // while a moving release is handled on the follow-up Custom event.
    class SelectionCopier {
    public:
        explicit SelectionCopier(ftxui::ScreenInteractive* screen)
            : _screen(screen)
        {
        }

        bool handle(Event& event, const ApplicationState& state)
        {
            if (event.is_mouse()) {
                const Mouse& m = event.mouse();
                if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
                    _active = true;
                    _moved  = false;
                    _x      = m.x;
                    _y      = m.y;
                } else if (_active && m.motion == Mouse::Moved) {
                    _moved = _moved || m.x != _x || m.y != _y;
                    _x     = m.x;
                    _y     = m.y;
                } else if (_active && m.button == Mouse::Left
                    && m.motion == Mouse::Released) {
                    _active = false;
                    if (!_moved) {
                        return false;
                    }
                    if (m.x == _x && m.y == _y) {
                        return _copy(state);
                    }
                    _release = true;
                } else if (m.button != Mouse::Left) {
                    _active = false;
                }
                return false;
            }
            if (event == Event::Custom && _release) {
                _release = false;
                _copy(state);
                return true;
            }
            return false;
        }

    private:
        bool _copy(const ApplicationState& state)
        {
            const std::string selected = _screen->GetSelection();
            if (selected.empty()) {
                return false;
            }
            copy_to_clipboard(*state.environment->system(), selected);
            return true;
        }

        ftxui::ScreenInteractive* _screen;
        bool _active  = false;
        bool _moved   = false;
        int _x        = 0;
        int _y        = 0;
        bool _release = false;
    };

    class Repl : public ComponentBase {
    public:
        Repl(std::shared_ptr<ApplicationState> state,
            ftxui::ScreenInteractive* screen)
            : _state(std::move(state))
            , _selection(screen)
        {
            const LayoutFn layout     = [this] { return _layout; };
            const WorkflowFn workflow = [this] { return _phase; };
            _side        = make_side_panel(_state, layout, workflow,
                [this](WorkflowPhase phase) { _set_phase(phase); });
            _status_line = make_status_line(_state, layout, workflow);
            _chat_hints.phase_line_fn = [this] {
                return _phase == WorkflowPhase::PLAN
                    ? std::string("Tab next phase · Shift+Tab previous "
                                  "phase · Ctrl+S chat")
                    : std::string(
                          "Tab next phase · Shift+Tab previous phase · Ctrl+S "
                          "Sidechat");
            };
            // The plan tab splits 50/50 once a document exists; the chat
            // renders its left half and must budget for those columns.
            _chat_hints.content_width = [this](const LayoutCtx& ctx) {
                return _phase == WorkflowPhase::PLAN && ctx.width > 0
                    ? ctx.width / 2
                    : review_content_width(ctx);
            };
            _chat      = make_chat(_state, layout, _chat_hints);
            _plan_tab  = make_plan_tab(_state, layout, _chat);
            _build_tab = make_build_tab(
                _state, layout, _chat, _sidechat, _sidechat_status);
            _review   = make_review(_state, layout,
                [this](WorkflowPhase phase) { _set_phase(phase); });
            _modal    = make_modal(_state);
            _sidechat = make_sidechat_component(
                _state, [this] { _focus_main(); }, _sidechat_status);

            _workspace_subscription
                = _state->environment->subscribe_to_workspace_change(
                    [] { animation::RequestAnimationFrame(); });
            _title_subscription = _state->session->subscribe_to_title_change(
                [] { animation::RequestAnimationFrame(); });
            // Flows can flip the session mode directly (/make-skill seeds
            // a Build turn); follow so the tabs and status line agree.
            _mode_subscription = _state->session->subscribe_to_mode_change(
                [this] {
                    _state->post([this] {
                        const Session::Mode mode  = _state->session->mode();
                        const WorkflowPhase phase = mode == Session::Mode::PLAN
                            ? WorkflowPhase::PLAN
                            : WorkflowPhase::BUILD;
                        if (_phase != phase
                            && _phase != WorkflowPhase::REVIEW) {
                            _set_phase(phase);
                        }
                    });
                });
            _phase    = _state->session->mode() == Session::Mode::PLAN
                ? WorkflowPhase::PLAN
                : WorkflowPhase::BUILD;
            _selected = static_cast<int>(_phase);
            _attach_chat();
            _review_available_cached = _review_available();
            _tab_names               = { "Plan", "Build" };
            if (_review_available_cached) {
                _tab_names.emplace_back("Review");
            }
            _tabs = CatchEvent(
                Menu(&_tab_names, &_selected, MenuOption::HorizontalAnimated()),
                [](const Event& event) {
                    return event == Event::Tab || event == Event::TabReverse;
                });
            _tabs_content = Container::Tab(
                { _plan_tab, _build_tab, _review }, &_selected);
            Add(Container::Stacked({
                Container::Vertical({ _tabs, _tabs_content }),
                _side,
                _status_line,
                _modal,
            }));
            _chat->TakeFocus();
        }

        Element OnRender() override
        {
            _sync_review_availability();
            _restore_focus_after_modal();
            const auto terminal_size = ftxui::Terminal::Size();
            _layout = layout_context(terminal_size.dimx, terminal_size.dimy);
            const int w       = _layout.width;
            Element side      = _side->Render();
            Element tab       = _tabs->Render();
            Element right_col = _tabs_content->Render();
            Element status    = _status_line->Render();

            const std::string title = _state->session->title();
            Element title_p = paragraph(title.empty() ? "New Session" : title)
                | bold | color(PANEL_FG);
            const bool narrow       = _layout.kind == LayoutCtx::Kind::NARROW;
            const bool side_by_side = _state->sidechat_open && !narrow
                && _phase != WorkflowPhase::PLAN;

            Element root;
            if (narrow && _sidechat_status.focused
                && _phase != WorkflowPhase::PLAN) {
                root = vbox({ _sidechat->Render() | flex, separatorEmpty(),
                           status })
                    | flex;
            } else if (_layout.kind == LayoutCtx::Kind::WIDE) {
                Element content = std::move(right_col) | xflex | yflex
                    | reflect(_main_pane_box);
                if (side_by_side) {
                    content = hbox({ std::move(content), separatorEmpty(),
                                  _sidechat->Render() })
                        | yflex;
                }
                Element main_panel
                    = vbox({ hbox({ text(" "), title_p | xflex, tab }),
                          std::move(content) })
                    | xflex | yflex;
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
            std::shared_ptr<Session> modal_session;
            if (_state->session->modal().index() != 0) {
                popup_source  = _modal;
                modal_session = _state->session;
            } else if (_sidechat_status.has_modal
                && _sidechat_status.has_modal()) {
                popup_source  = _sidechat_status.modal;
                modal_session = _state->sidechat ? _state->sidechat->session
                                                 : _state->session;
            }
            if (popup_source) {
                const int h = terminal_size.dimy;
                const int mw
                    = std::min(w - 4, modal_max_width(modal_session->modal()));
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
                _state->on_exit();
                return true;
            }
            if (_selection.handle(event, *_state)) {
                return true;
            }
            if (_state->session->modal().index() != 0) {
                return _modal->OnEvent(event);
            }
            if (_sidechat_status.has_modal && _sidechat_status.has_modal()) {
                return _sidechat->OnEvent(event);
            }
            // The sidechat receives all events while visible; hidden, only
            // the toggle shortcut — stale clicks on its last-rendered box
            // must not reach it.
            const bool sidechat_visible = _state->sidechat_open
                && _phase != WorkflowPhase::PLAN
                && (_layout.kind != LayoutCtx::Kind::NARROW
                    || _sidechat_status.focused);
            if (sidechat_visible && _sidechat->OnEvent(event)) {
                return true;
            }
            if (is_sidechat_toggle(event) && _phase != WorkflowPhase::PLAN
                && _sidechat->OnEvent(event)) {
                return true;
            }
            // PLAN: the tab swaps doc/chat on Ctrl+S (no sidechat there).
            if (is_sidechat_toggle(event) && _phase == WorkflowPhase::PLAN
                && _tabs_content->OnEvent(event)) {
                return true;
            }
            if (event == Event::Tab) {
                _set_phase(
                    next_workflow_phase(_phase, _review_available_cached));
                return true;
            }
            if (is_reverse_tab(event)) {
                _set_phase(
                    previous_workflow_phase(_phase, _review_available_cached));
                return true;
            }
            if (event.is_mouse()) {
                const Mouse& m = event.mouse();
                if (m.button == Mouse::Left && m.motion == Mouse::Pressed
                    && _state->sidechat_open && _sidechat_status.focused
                    && _main_pane_box.Contain(m.x, m.y)) {
                    _focus_main();
                }
                const int previous = _selected;
                if (_tabs->OnEvent(event)) {
                    if (_selected != previous) {
                        _set_phase(static_cast<WorkflowPhase>(_selected));
                    }
                    return true;
                }
                if (_side->OnEvent(event)) {
                    return true;
                }
            }
            return _tabs_content->OnEvent(event);
        }

        void OnAnimation(animation::Params& params) override
        {
            ComponentBase::OnAnimation(params);
            _sidechat->OnAnimation(params);
        }

        Component ActiveChild() override
        {
            if (_state->sidechat_open && _sidechat_status.focused
                && _phase != WorkflowPhase::PLAN) {
                return _sidechat;
            }
            return ComponentBase::ActiveChild();
        }

        bool _review_available() const
        {
            const auto& environment = _state->environment;
            return environment->ready() && environment->system()->has_git
                && environment->workspace()->project_root.has_value();
        }

        void _sync_review_availability()
        {
            const bool available = _review_available();
            if (available == _review_available_cached) {
                return;
            }
            _review_available_cached = available;
            _tab_names               = { "Plan", "Build" };
            if (_review_available_cached) {
                _tab_names.push_back("Review");
                return;
            }
            if (_phase == WorkflowPhase::REVIEW) {
                _set_phase(WorkflowPhase::PLAN);
            }
        }

        void _set_phase(WorkflowPhase phase)
        {
            _phase    = phase;
            _selected = static_cast<int>(phase);
            if (const auto mode = workflow_mode(phase)) {
                _state->session->set_mode(*mode);
            }
            if (phase != WorkflowPhase::REVIEW) {
                _attach_chat();
            }
            // The sidechat hides in PLAN; keeping focus on a hidden pane
            // would swallow all input.
            if (phase == WorkflowPhase::PLAN || !_sidechat_status.focused) {
                _focus_main();
            }
        }

        void _focus_main()
        {
            if (_selected == static_cast<int>(WorkflowPhase::REVIEW)) {
                _review->TakeFocus();
            } else {
                _chat->TakeFocus();
            }
        }

        // Buttons that open modals (sidebar links, review actions) call
        // TakeFocus, rotating the shared Stacked's active child away from
        // the main column. After the modal closes the rotation persists:
        // the main tree reports unfocused and keystrokes die.
        void _restore_focus_after_modal()
        {
            const bool open = _state->session->modal().index() != 0;
            if (_modal_was_open && !open) {
                if (_phase == WorkflowPhase::PLAN) {
                    _plan_tab->TakeFocus();
                } else {
                    _focus_main();
                }
            }
            _modal_was_open = open;
        }

        // Add detaches from the previous tab: one parent at all times.
        void _attach_chat()
        {
            if (_phase == WorkflowPhase::BUILD) {
                _build_tab->Add(_chat);
            } else {
                _plan_tab->Add(_chat);
            }
        }

        std::shared_ptr<ApplicationState> _state;
        SelectionCopier _selection;
        ftxui::Component _sidechat;
        SidechatStatus _sidechat_status;
        Component _side;
        Component _modal;
        Component _status_line;
        Component _chat;
        Component _plan_tab;
        Component _build_tab;
        Component _review;
        ChatHints _chat_hints;
        Component _tabs_content;
        Component _tabs;
        Signal<>::Subscription _workspace_subscription;
        Signal<>::Subscription _title_subscription;
        Signal<>::Subscription _mode_subscription;
        LayoutCtx _layout = layout_context(0);
        WorkflowPhase _phase { WorkflowPhase::PLAN };
        std::vector<std::string> _tab_names;
        bool _review_available_cached { false };
        int _selected { 0 };
        ftxui::Box _main_pane_box { };
        bool _modal_was_open = false;
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
    auto app = ftxui::Make<Repl>(state, &screen);
    // Empty callback: FTXUI only posts Event::Custom after selection changes
    // when one is registered; SelectionCopier consumes it.
    screen.SelectionChange([] { });
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
