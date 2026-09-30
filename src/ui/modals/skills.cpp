#include "app/flows.h"
#include "common/modal.h"
#include "common/types.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    class SkillsView : public ComponentBase {
    public:
        SkillsView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
        {
            _entries = std::get<SkillsModal>(_state->session->modal()).entries;
        }

        Element OnRender() override
        {
            Elements rows = modal_header("Skills");
            if (_entries.empty()) {
                rows.push_back(text("No skills discovered") | dim);
            }
            std::string previous_scope;
            for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
                const auto& entry = _entries[i];
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
                if (i == _cursor) {
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
                imza::close_modal(*_state);
                return true;
            }
            if (move_list_cursor(
                    event, _cursor, static_cast<int>(_entries.size()))) {
                return true;
            }
            if (!_entries.empty()
                && (event == Event::ArrowLeft || event == Event::ArrowRight
                    || event == Event::Character(' '))) {
                int value = static_cast<int>(_entries[_cursor].policy);
                value     = event == Event::ArrowLeft ? (value + 2) % 3
                                                      : (value + 1) % 3;
                _entries[_cursor].policy = static_cast<SkillPolicy>(value);
                return true;
            }
            if (event == Event::Return) {
                SkillPolicyChanges changes;
                for (const auto& entry : _entries) {
                    changes.entries.push_back(
                        { entry.name, entry.project_root, entry.policy });
                }
                imza::resolve_modal(
                    *_state, ModalResult { std::move(changes) });
                return true;
            }
            return false;
        }

    private:
        std::shared_ptr<ApplicationState> _state;
        std::vector<SkillsModal::Entry> _entries;
        int _cursor = 0;
    };

} // namespace

ftxui::Component make_skills(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SkillsView>(state);
}

} // namespace imza
