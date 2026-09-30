#include "app/flows.h"
#include "common/modal.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    class SessionsView : public ComponentBase {
    public:
        SessionsView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
            , _session(_state->session)
        {
            _store_subscription = _state->sessions->subscribe(
                [] { animation::RequestAnimationFrame(); });
            _filter_input = Input(field_option(
                &_filter_text, &_filter_cursor, "filter sessions", [this] {
                    _selected = 0;
                    _refill();
                }));
            _refresh();
            _filter_input->TakeFocus();
        }

        Element OnRender() override
        {
            _refresh();
            Elements rows              = modal_header("Sessions");
            const bool loading_blocked = _session->has_pending_work();
            if (!_state->sessions->ready()) {
                rows.push_back(text("Loading sessions…") | dim);
            } else if (_sessions.empty()) {
                rows.push_back(text("No saved sessions") | dim);
            } else if (_confirming) {
                rows.push_back(text("Delete “" + _row().title + "”?") | bold);
                rows.push_back(separatorEmpty());
                rows.push_back(hint_bar("Enter delete · Esc cancel"));
            } else {
                rows.push_back(_filter_input->Render() | xflex);
                if (_visible.empty()) {
                    rows.push_back(text("no matching sessions") | dim);
                }
                for (int i = 0; i < static_cast<int>(_visible.size()); ++i) {
                    const SavedSession& entry = _sessions[_visible[i]];
                    const bool locked         = _locked.contains(entry.path);
                    Element row               = hbox({
                        text(i == _selected ? "› " : "  "),
                        text(entry.title),
                        filler(),
                        text(locked ? "locked" : entry.saved_at)
                            | color(PANEL_FG_DIM),
                    });
                    if (locked) {
                        row = std::move(row) | dim;
                    }
                    if (i == _selected) {
                        row = std::move(row) | bold;
                    }
                    rows.push_back(std::move(row));
                }
                rows.push_back(separatorEmpty());
                if (loading_blocked) {
                    rows.push_back(
                        text("Finish or interrupt pending work before loading")
                        | color(PANEL_FG));
                }
                rows.push_back(hint_bar(loading_blocked
                        ? "type filter · ↑↓ rows · DEL delete · Esc close"
                        : "type filter · ↑↓ rows · Enter load · DEL delete · "
                          "Esc close"));
            }
            return vbox({ vbox(std::move(rows)), separatorEmpty() }) | xflex;
        }

        bool OnEvent(Event event) override
        {
            _refresh();
            if (_confirming) {
                if (event == Event::Escape) {
                    _confirming = false;
                    return true;
                }
                if (event == Event::Return) {
                    imza::delete_saved_session(*_state, _row().path);
                    return true;
                }
                return true;
            }
            if (event == Event::Escape) {
                imza::close_modal(*_state);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                move_list_cursor(
                    event, _selected, static_cast<int>(_visible.size()));
                return true;
            }
            if (event == Event::Delete && !_visible.empty()) {
                _confirming = true;
                return true;
            }
            if (event == Event::Return && !_visible.empty()) {
                if (_session->has_pending_work() || _row_locked()) {
                    return true;
                }
                imza::resolve_modal(*_state, ModalResult { _row().path });
                return true;
            }
            return _filter_input->OnEvent(event);
        }

    private:
        const SavedSession& _row() const
        {
            return _sessions[_visible[static_cast<std::size_t>(_selected)]];
        }

        bool _row_locked() const { return _locked.contains(_row().path); }

        void _refill()
        {
            _visible = filter_visible(_filter_text, _sessions.size(),
                [this](std::size_t index) { return _sessions[index].title; });
        }

        void _refresh()
        {
            const auto sessions = _state->sessions->sessions();
            if (sessions == _last_sessions) {
                return;
            }
            _last_sessions = sessions;
            _sessions      = std::move(sessions);
            _locked.clear();
            for (const SavedSession& entry : _sessions) {
                if (_state->sessions->is_locked(entry.path)) {
                    _locked.insert(entry.path);
                }
            }
            _selected = 0;
            _refill();
        }

        std::shared_ptr<ApplicationState> _state;
        std::shared_ptr<Session> _session;
        Signal<>::Subscription _store_subscription;
        std::vector<SavedSession> _sessions;
        std::vector<SavedSession> _last_sessions;
        std::vector<std::size_t> _visible;
        std::set<std::filesystem::path> _locked;
        std::string _filter_text;
        int _filter_cursor = 0;
        int _selected      = 0;
        Component _filter_input;
        bool _confirming = false;
    };

} // namespace

ftxui::Component make_sessions(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SessionsView>(state);
}

} // namespace imza
