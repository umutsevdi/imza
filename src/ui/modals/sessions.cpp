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
            : state_(std::move(state))
            , session_(state_->session)
        {
            store_subscription_ = state_->sessions->subscribe(
                [] { animation::RequestAnimationFrame(); });
            filter_input_ = Input(field_option(
                &filter_text_, &filter_cursor_, "filter sessions", [this] {
                    selected_ = 0;
                    _refill();
                }));
            _refresh();
            filter_input_->TakeFocus();
        }

        Element OnRender() override
        {
            _refresh();
            Elements rows              = modal_header("Sessions");
            const bool loading_blocked = session_->has_pending_work();
            if (!state_->sessions->ready()) {
                rows.push_back(text("Loading sessions…") | dim);
            } else if (sessions_.empty()) {
                rows.push_back(text("No saved sessions") | dim);
            } else if (confirming_) {
                rows.push_back(text("Delete “" + _row().title + "”?") | bold);
                rows.push_back(separatorEmpty());
                rows.push_back(hint_bar("Enter delete · Esc cancel"));
            } else {
                rows.push_back(filter_input_->Render() | xflex);
                if (visible_.empty()) {
                    rows.push_back(text("no matching sessions") | dim);
                }
                for (int i = 0; i < static_cast<int>(visible_.size()); ++i) {
                    const SavedSession& entry = sessions_[visible_[i]];
                    const bool locked         = locked_.contains(entry.path);
                    Element row               = hbox({
                        text(i == selected_ ? "› " : "  "),
                        text(entry.title),
                        filler(),
                        text(locked ? "locked" : entry.saved_at)
                            | color(PANEL_FG_DIM),
                    });
                    if (locked) {
                        row = std::move(row) | dim;
                    }
                    if (i == selected_) {
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
            if (confirming_) {
                if (event == Event::Escape) {
                    confirming_ = false;
                    return true;
                }
                if (event == Event::Return) {
                    imza::delete_saved_session(*state_, _row().path);
                    return true;
                }
                return true;
            }
            if (event == Event::Escape) {
                imza::close_modal(*state_);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                move_list_cursor(
                    event, selected_, static_cast<int>(visible_.size()));
                return true;
            }
            if (event == Event::Delete && !visible_.empty()) {
                confirming_ = true;
                return true;
            }
            if (event == Event::Return && !visible_.empty()) {
                if (session_->has_pending_work() || _row_locked()) {
                    return true;
                }
                imza::resolve_modal(*state_, ModalResult { _row().path });
                return true;
            }
            return filter_input_->OnEvent(event);
        }

    private:
        const SavedSession& _row() const
        {
            return sessions_[visible_[static_cast<std::size_t>(selected_)]];
        }

        bool _row_locked() const { return locked_.contains(_row().path); }

        void _refill()
        {
            visible_ = filter_visible(filter_text_, sessions_.size(),
                [this](std::size_t index) { return sessions_[index].title; });
        }

        void _refresh()
        {
            const auto sessions = state_->sessions->sessions();
            if (sessions == last_sessions_) {
                return;
            }
            last_sessions_ = sessions;
            sessions_      = std::move(sessions);
            locked_.clear();
            for (const SavedSession& entry : sessions_) {
                if (state_->sessions->is_locked(entry.path)) {
                    locked_.insert(entry.path);
                }
            }
            selected_ = 0;
            _refill();
        }

        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        Signal<>::Subscription store_subscription_;
        std::vector<SavedSession> sessions_;
        std::vector<SavedSession> last_sessions_;
        std::vector<std::size_t> visible_;
        std::set<std::filesystem::path> locked_;
        std::string filter_text_;
        int filter_cursor_ = 0;
        int selected_      = 0;
        Component filter_input_;
        bool confirming_ = false;
    };

} // namespace

ftxui::Component make_sessions(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SessionsView>(state);
}

} // namespace imza
