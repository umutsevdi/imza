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
            _refresh();
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
                rows.push_back(
                    text("Delete “" + sessions_[cursor_].title + "”?") | bold);
                rows.push_back(separatorEmpty());
                rows.push_back(hint_bar("Enter delete · Esc cancel"));
            } else {
                for (int index = 0; index < static_cast<int>(sessions_.size());
                    ++index) {
                    Element row = hbox({ text(sessions_[index].title), filler(),
                        text(sessions_[index].saved_at)
                            | color(PANEL_FG_DIM) });
                    if (index == cursor_) {
                        row = std::move(row) | bgcolor(PANEL_COLOR_FOCUS)
                            | bold;
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
                        ? "↑↓ rows · d delete · Esc close"
                        : "↑↓ rows · Enter load · d delete · Esc close"));
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
                    imza::delete_saved_session(
                        *state_, sessions_[cursor_].path);
                    return true;
                }
                return true;
            }
            if (event == Event::Escape) {
                imza::close_modal(*state_);
                return true;
            }
            if (move_list_cursor(
                    event, cursor_, static_cast<int>(sessions_.size()))) {
                return true;
            }
            if ((event == Event::Character('d')
                    || event == Event::Character('D'))
                && !sessions_.empty()) {
                confirming_ = true;
                return true;
            }
            if (event == Event::Return && !sessions_.empty()) {
                if (session_->has_pending_work()) {
                    return true;
                }
                imza::resolve_modal(
                    *state_, ModalResult { sessions_[cursor_].path });
                return true;
            }
            return false;
        }

    private:
        void _refresh()
        {
            const auto sessions = state_->sessions->sessions();
            if (sessions == last_sessions_) {
                return;
            }
            last_sessions_ = sessions;
            sessions_      = sessions;
            if (sessions_.empty()) {
                cursor_ = 0;
            } else {
                cursor_ = std::clamp(
                    cursor_, 0, static_cast<int>(sessions_.size()) - 1);
            }
        }

        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        Signal<>::Subscription store_subscription_;
        std::vector<SavedSession> last_sessions_;
        std::vector<SavedSession> sessions_;
        int cursor_      = 0;
        bool confirming_ = false;
    };

} // namespace

ftxui::Component make_sessions(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SessionsView>(state);
}

} // namespace imza
