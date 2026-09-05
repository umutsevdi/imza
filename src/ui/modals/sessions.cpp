#include "agent/flows.h"
#include "common/modal.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    class SessionsView : public ComponentBase {
    public:
        SessionsView(std::shared_ptr<ApplicationState> state)
            : state_(std::move(state))
            , session_(state_->session)
        {
            const auto modal = std::get<SessionsModal>(session_->modal());
            titles_          = modal.titles;
            saved_at_        = modal.saved_at;
            paths_           = modal.paths;
        }

        Element OnRender() override
        {
            Elements rows { text("Sessions") | bold, separatorEmpty() };
            const bool loading_blocked = session_->has_pending_work();
            if (titles_.empty()) {
                rows.push_back(text("No saved sessions") | dim);
            } else if (confirming_) {
                rows.push_back(
                    text("Delete “" + titles_[cursor_] + "”?") | bold);
                rows.push_back(separatorEmpty());
                rows.push_back(hbox(
                    { filler(), text("Enter delete · Esc cancel") | dim }));
            } else {
                for (int index = 0; index < static_cast<int>(titles_.size());
                    ++index) {
                    Element row = hbox({ text(titles_[index]), filler(),
                        text(saved_at_[index]) | color(PANEL_FG_DIM) });
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
            if (confirming_) {
                if (event == Event::Escape) {
                    confirming_ = false;
                    return true;
                }
                if (event == Event::Return) {
                    ursa::delete_saved_session(*state_, paths_[cursor_]);
                    return true;
                }
                return true;
            }
            if (event == Event::Escape) {
                ursa::close_modal(*state_);
                return true;
            }
            if (move_list_cursor(
                    event, cursor_, static_cast<int>(titles_.size()))) {
                return true;
            }
            if ((event == Event::Character('d')
                    || event == Event::Character('D'))
                && !titles_.empty()) {
                confirming_ = true;
                return true;
            }
            if (event == Event::Return && !paths_.empty()) {
                if (session_->has_pending_work()) {
                    return true;
                }
                ursa::resolve_modal(*state_,
                    ModalResult { std::filesystem::path(paths_[cursor_]) });
                return true;
            }
            return false;
        }

    private:
        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        std::vector<std::string> titles_;
        std::vector<std::string> saved_at_;
        std::vector<std::string> paths_;
        int cursor_      = 0;
        bool confirming_ = false;
    };

} // namespace

ftxui::Component make_sessions(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SessionsView>(state);
}

} // namespace ursa
