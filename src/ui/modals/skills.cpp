#include "agent/flows.h"
#include "common/modal.h"
#include "common/types.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    class SkillsView : public ComponentBase {
    public:
        SkillsView(std::shared_ptr<ApplicationState> state)
            : state_(std::move(state))
            , session_(state_->session)
        {
            entries_ = std::get<SkillsModal>(session_->modal()).entries;
        }

        Element OnRender() override
        {
            Elements rows { text("Skills") | bold, separatorEmpty() };
            if (entries_.empty()) {
                rows.push_back(text("No skills discovered") | dim);
            }
            std::string previous_scope;
            for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
                const auto& entry = entries_[i];
                const std::string scope
                    = entry.project_root.empty() ? "Global" : "Project";
                if (scope != previous_scope) {
                    rows.push_back(section_title(scope + " skills"));
                    previous_scope = scope;
                }
                const auto choice
                    = [&entry](SkillPolicy policy, std::string label) {
                          const bool selected = entry.policy == policy;
                          return choice_label(
                              choice_marker(false, selected) + std::move(label),
                              selected, false);
                      };
                Elements content {
                    hbox({ text(entry.name) | bold, filler(),
                        choice(SkillPolicy::ALLOW, "Allow"), text("  "),
                        choice(SkillPolicy::ASK, "Ask"), text("  "),
                        choice(SkillPolicy::DENY, "Deny") }),
                };
                if (!entry.description.empty()) {
                    content.push_back(
                        text(fit(entry.description, 76)) | color(PANEL_FG_DIM));
                }
                Element row = vbox(std::move(content));
                if (i == cursor_) {
                    row = std::move(row) | bgcolor(PANEL_COLOR_FOCUS) | focus;
                }
                rows.push_back(std::move(row));
            }
            rows.push_back(separatorEmpty());
            rows.push_back(
                hint_bar("↑↓ rows · ←→ policy · Enter save · Esc close"));
            return vbox({ vbox(std::move(rows)) | vscroll_indicator | frame,
                       separatorEmpty() })
                | xflex;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::Escape) {
                ursa::close_modal(*state_);
                return true;
            }
            if (move_list_cursor(
                    event, cursor_, static_cast<int>(entries_.size()))) {
                return true;
            }
            if (!entries_.empty()
                && (event == Event::ArrowLeft || event == Event::ArrowRight
                    || event == Event::Character(' '))) {
                int value = static_cast<int>(entries_[cursor_].policy);
                value     = event == Event::ArrowLeft ? (value + 2) % 3
                                                      : (value + 1) % 3;
                entries_[cursor_].policy = static_cast<SkillPolicy>(value);
                return true;
            }
            if (event == Event::Return) {
                SkillPolicyChanges changes;
                for (const auto& entry : entries_) {
                    changes.entries.push_back(
                        { entry.name, entry.project_root, entry.policy });
                }
                ursa::resolve_modal(
                    *state_, ModalResult { std::move(changes) });
                return true;
            }
            return false;
        }

    private:
        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        std::vector<SkillsModal::Entry> entries_;
        int cursor_ = 0;
    };

} // namespace

ftxui::Component make_skills(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SkillsView>(state);
}

} // namespace ursa
