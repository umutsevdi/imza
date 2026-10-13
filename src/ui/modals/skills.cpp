#include "app/flows.h"
#include "common/modal.h"
#include "common/types.h"
#include "common/util.h"
#include "tools/skill_registry.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    constexpr int COL_NAME  = 22;
    constexpr int COL_SCOPE = 8;
    constexpr int COL_DESC  = 26;
    // Wait this long after the last keystroke before querying skills.sh.
    constexpr auto SEARCH_DEBOUNCE = std::chrono::milliseconds(350);

    const std::vector<std::string>& policy_names()
    {
        static const std::vector<std::string> names { "Allow", "Ask", "Deny" };
        return names;
    }

    SkillPolicy policy_at(int index)
    {
        switch (index) {
        case 0: return SkillPolicy::ALLOW;
        case 2: return SkillPolicy::DENY;
        default: return SkillPolicy::ASK;
        }
    }

    int policy_index(SkillPolicy policy)
    {
        switch (policy) {
        case SkillPolicy::ALLOW: return 0;
        case SkillPolicy::DENY: return 2;
        default: return 1;
        }
    }

    class SkillsView : public ComponentBase {
    public:
        SkillsView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
        {
            _entries = std::get<SkillsModal>(_state->session->modal()).entries;
            _installed = _installed_names();
            if (_state->skill_registry) {
                _subscription = _state->skill_registry->subscribe(
                    [] { animation::RequestAnimationFrame(); });
            }
            _tab_names = { "Manage", "Search" };
            // The tab bar is rendered directly, so it has no parent to
            // decide focus and would otherwise always report focused,
            // inverting the selected tab. Style it without the inverted
            // focus state; the animated underline is unaffected.
            MenuOption tab_option = MenuOption::HorizontalAnimated();
            tab_option.entries_option.transform = [](const EntryState& state) {
                Element e = text(state.label);
                return state.active ? e | bold : e | dim;
            };
            _tabs         = Menu(&_tab_names, &_tab, std::move(tab_option));
            _search_input = Input(field_option(
                &_query, &_query_cursor, "search skills.sh", [this] {
                    _results.clear();
                    _marked.clear();
                    _search_selected = 0;
                    _search_due
                        = std::chrono::steady_clock::now() + SEARCH_DEBOUNCE;
                }));
            _rebuild_manage();
        }

        Element OnRender() override
        {
            _installed = _installed_names();
            _refresh_entries();
            Elements rows = modal_header("Skills");
            rows.push_back(_tabs->Render());
            rows.push_back(separatorEmpty());
            rows.push_back(
                (_tab == 0 ? _render_manage() : _render_search()) | yflex);
            rows.push_back(separatorEmpty());
            rows.push_back(hint_bar(_tab == 0
                    ? "Tab search · ↑↓ rows · ←→ control · Space/Enter "
                      "activate · DEL remove · Esc close"
                    : "Tab manage · type to search · Enter install · "
                      "↑↓ rows · DEL remove · Esc close"));
            return vbox(std::move(rows)) | xflex;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::Escape) {
                if (_policy_open_row >= 0) {
                    _policy_open_row = -1;
                    return true;
                }
                if (_confirming()) {
                    _confirm.clear();
                    _rebuild_manage();
                    return true;
                }
                imza::close_modal(*_state);
                return true;
            }
            if (event == Event::Tab || event == Event::TabReverse) {
                _tab = _tab == 0 ? 1 : 0;
                _confirm.clear();
                _policy_open_row = -1;
                _rebuild_manage();
                if (_tab == 1) {
                    _search_input->TakeFocus();
                }
                return true;
            }
            return _tab == 0 ? _handle_manage(event) : _handle_search(event);
        }

        // The tab bar and the manage rows are rendered directly rather than
        // attached as children, so animation frames (the tab underline)
        // must be forwarded by hand or they never advance.
        void OnAnimation(animation::Params& params) override
        {
            if (_tabs) {
                _tabs->OnAnimation(params);
            }
            if (_rows_container) {
                _rows_container->OnAnimation(params);
            }
        }

    private:
        // ---- Manage tab -------------------------------------------------

        bool _confirming() const
        {
            return std::ranges::any_of(
                _confirm, [](const auto& entry) { return entry.second; });
        }

        bool _handle_manage(const Event& event)
        {
            if (_policy_open_row >= 0) {
                return _handle_policy(event);
            }
            if (_confirming()) {
                return _rows_container ? _rows_container->OnEvent(event)
                                       : false;
            }
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                _row_move(event == Event::ArrowDown ? 1 : -1);
                return true;
            }
            if (event == Event::Delete && !_entries.empty()) {
                _confirm[_row_selected] = true;
                _rebuild_manage();
                return true;
            }
            // Enter and Space both activate the focused control (the policy
            // chip or the Remove button), matching button behavior; the
            // row's Left/Right keys choose which one is focused.
            return _rows_container ? _rows_container->OnEvent(event) : false;
        }

        // The open policy list: arrows move the highlight, Enter commits.
        bool _handle_policy(const Event& event)
        {
            if (event == Event::ArrowDown) {
                _policy_cursor = std::min(_policy_cursor + 1, 2);
                return true;
            }
            if (event == Event::ArrowUp) {
                _policy_cursor = std::max(_policy_cursor - 1, 0);
                return true;
            }
            if (event == Event::Return || event == Event::Character(' ')) {
                _commit_policy();
                return true;
            }
            return true; // swallow everything while the list is open
        }

        void _open_policy(int row)
        {
            if (row < 0 || row >= static_cast<int>(_entries.size())) {
                return;
            }
            _policy_open_row = row;
            _policy_cursor
                = policy_index(_entries[static_cast<std::size_t>(row)].policy);
        }

        void _commit_policy()
        {
            if (_policy_open_row >= 0
                && _policy_open_row < static_cast<int>(_entries.size())) {
                _entries[static_cast<std::size_t>(_policy_open_row)].policy
                    = policy_at(_policy_cursor);
                _persist_policies();
            }
            _policy_open_row = -1;
            _rebuild_manage();
        }

        void _row_move(int delta)
        {
            if (_entries.empty()) {
                return;
            }
            _row_selected = std::clamp(_row_selected + delta, 0,
                static_cast<int>(_entries.size()) - 1);
            if (_row_selected < static_cast<int>(_row_buttons.size())) {
                _row_buttons[static_cast<std::size_t>(_row_selected)]
                    ->TakeFocus();
            }
        }

        // Persists the current policy set; the modal applies each change
        // immediately so Esc can never discard an edit.
        void _persist_policies()
        {
            SkillPolicyChanges changes;
            for (const auto& entry : _entries) {
                changes.entries.push_back(
                    { entry.name, entry.project_root, entry.policy });
            }
            if (!_state->providers->set_skill_policies(changes)) {
                _state->session->set_error("Failed to save skill policies.");
            }
        }

        void _remove(int index)
        {
            _confirm.erase(index);
            if (index < 0 || index >= static_cast<int>(_entries.size())) {
                return;
            }
            _remove_by_name(_entries[static_cast<std::size_t>(index)].name);
            _row_selected = 0;
        }

        void _remove_by_name(const std::string& name)
        {
            if (!_state->skill_registry) {
                return;
            }
            for (const Skill& skill : _state->environment->skills()) {
                if (skill.name == name) {
                    _state->skill_registry->remove_path(skill.path);
                    return;
                }
            }
        }

        // Rebuilds the row list from the discovered catalogue, picking up
        // skills installed or removed since the modal opened.
        void _refresh_entries()
        {
            const std::vector<Skill> catalog = _state->environment->skills();
            const Config config              = _state->providers->config();
            std::map<std::string, SkillPolicy> edited;
            for (const auto& entry : _entries) {
                edited[entry.project_root + "\n" + entry.name] = entry.policy;
            }
            std::vector<SkillsModal::Entry> fresh;
            fresh.reserve(catalog.size());
            for (const Skill& skill : catalog) {
                const std::string root
                    = skill.scope == Skill::Scope::PROJECT && skill.project_root
                    ? skill.project_root->string()
                    : std::string { };
                const auto found = edited.find(root + "\n" + skill.name);
                fresh.push_back({ skill.name, skill.description, root,
                    found != edited.end() ? found->second
                                          : skill_policy(config, skill) });
            }
            if (fresh == _entries) {
                return;
            }
            _entries         = std::move(fresh);
            _row_selected    = std::clamp(_row_selected, 0,
                std::max(0, static_cast<int>(_entries.size()) - 1));
            _policy_open_row = -1;
            _confirm.clear();
            _rebuild_manage();
        }

        void _rebuild_manage()
        {
            _row_buttons.clear();
            Components rows;
            for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
                rows.push_back(_make_row(i));
            }
            _rows_container = Container::Vertical(std::move(rows));
            if (!_row_buttons.empty()) {
                _row_buttons[static_cast<std::size_t>(std::min(_row_selected,
                                 static_cast<int>(_row_buttons.size()) - 1))]
                    ->TakeFocus();
            }
        }

        Component _make_row(int index)
        {
            const bool is_confirm
                = _confirm.count(index) != 0 && _confirm.at(index);

            // Only the controls are focusable children, so Left/Right moves
            // between the policy chip and Remove; the label is drawn by the
            // row renderer and never takes focus.
            Components controls;
            if (is_confirm) {
                controls.push_back(
                    action_button("Yes", [this, index] { _remove(index); }));
                controls.push_back(action_button("No", [this, index] {
                    _confirm.erase(index);
                    _rebuild_manage();
                }));
            } else {
                controls.push_back(inline_link_button(
                    [this, index] {
                        const auto& entry
                            = _entries[static_cast<std::size_t>(index)];
                        return text(policy_names()[static_cast<std::size_t>(
                                        policy_index(entry.policy))]
                            + " ▾");
                    },
                    [this, index] { _open_policy(index); }));
                controls.push_back(action_button("Remove", [this, index] {
                    _confirm[index] = true;
                    _rebuild_manage();
                }));
            }
            Component row = Container::Horizontal(std::move(controls));
            _row_buttons.push_back(row);

            return Renderer(row, [this, row, index, is_confirm] {
                Element label = text("");
                if (index >= 0 && index < static_cast<int>(_entries.size())) {
                    const auto& entry
                        = _entries[static_cast<std::size_t>(index)];
                    const std::string scope
                        = entry.project_root.empty() ? "Global" : "Project";
                    label = hbox({ text(fit(entry.name, COL_NAME)) | bold,
                        text(fit(scope, COL_SCOPE)) | color(PANEL_FG_DIM),
                        text(fit(entry.description, COL_DESC))
                            | color(PANEL_FG_DIM) });
                }
                Element line = hbox(
                    { std::move(label), filler(), row->Render() | xflex });
                if (is_confirm || index == _row_selected) {
                    line |= bgcolor(PANEL_COLOR_FOCUS);
                }
                if (index != _policy_open_row) {
                    return line;
                }
                // The open policy list renders below the row, never inside
                // it, so the row (and its Remove button) stays one line.
                return vbox({ std::move(line), _policy_list() });
            });
        }

        Element _policy_list()
        {
            const std::string pad(COL_NAME + COL_SCOPE, ' ');
            Elements items;
            for (int p = 0; p < static_cast<int>(policy_names().size()); ++p) {
                const bool highlighted = p == _policy_cursor;
                Element e              = text(pad + (highlighted ? "› " : "  ")
                    + policy_names()[static_cast<std::size_t>(p)]);
                if (highlighted) {
                    e |= bold | color(PANEL_FG);
                } else {
                    e |= color(PANEL_FG_DIM);
                }
                items.push_back(std::move(e));
            }
            return vbox(std::move(items));
        }

        Element _render_manage()
        {
            if (_entries.empty()) {
                return text("No skills discovered") | dim;
            }
            return _rows_container->Render() | vscroll_indicator | frame;
        }

        // ---- Search tab -------------------------------------------------

        Element _render_search()
        {
            _maybe_fire_search();
            const SkillRegistry::Snapshot snap = _snapshot();
            _sync_results(snap);

            Elements rows;
            rows.push_back(_search_input->Render() | xflex);

            // Report only the current query's state: a previous query's
            // count must not linger next to the new (empty) list.
            std::string status;
            bool ok = true;
            if (snap.phase == SkillRegistry::Phase::SEARCHING
                && snap.query == _query) {
                status = "searching…";
            } else if (snap.query == _query) {
                status = snap.status;
                ok     = snap.status_ok;
            }
            if (!status.empty()) {
                rows.push_back(status_text(status, ok));
            }
            if (_results.empty()) {
                if (_query.size() < 2) {
                    rows.push_back(text("type to search skills.sh") | dim);
                } else if (snap.phase == SkillRegistry::Phase::IDLE
                    && snap.status_ok && snap.query == _query) {
                    rows.push_back(text("no matches") | dim);
                }
            }
            for (int i = 0; i < static_cast<int>(_results.size()); ++i) {
                rows.push_back(_search_row(i));
            }
            return vbox(std::move(rows)) | vscroll_indicator | frame;
        }

        Element _search_row(int index)
        {
            const RemoteSkill& skill
                = _results[static_cast<std::size_t>(index)];
            const bool installed = _installed.contains(skill.slug);
            // Same row shape as the model and connection pickers.
            ModelRow row;
            row.model_id = skill.slug;
            row.name     = skill.name;
            row.tag      = skill.source
                + (installed ? " · ✓ installed"
                             : " · " + compact_number(skill.installs));
            Element line = model_picker_row(row, index == _search_selected);
            if (index == _search_selected) {
                line |= bgcolor(PANEL_COLOR_FOCUS);
            }
            return line;
        }

        bool _handle_search(const Event& event)
        {
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                if (!_results.empty()) {
                    _search_selected = std::clamp(
                        _search_selected + (event == Event::ArrowDown ? 1 : -1),
                        0, static_cast<int>(_results.size()) - 1);
                }
                return true;
            }
            if (event == Event::Return && !_results.empty()) {
                _install(_results[static_cast<std::size_t>(_search_selected)]);
                return true;
            }
            if (event == Event::Delete && !_results.empty()) {
                _remove_by_name(
                    _results[static_cast<std::size_t>(_search_selected)].slug);
                return true;
            }
            return _search_input->OnEvent(event);
        }

        // Fires the debounced query once the user pauses typing; the
        // animation frame keeps the render loop alive until then.
        void _maybe_fire_search()
        {
            if (_search_due == std::chrono::steady_clock::time_point { }) {
                return;
            }
            if (std::chrono::steady_clock::now() < _search_due) {
                animation::RequestAnimationFrame();
                return;
            }
            _search_due = { };
            _run_search();
        }

        void _run_search()
        {
            if (_query.size() < 2 || _query == _searched) {
                return;
            }
            _searched        = _query;
            _search_selected = 0;
            if (_state->skill_registry) {
                _state->skill_registry->search(_query);
            }
        }

        void _install(const RemoteSkill& skill)
        {
            if (!_installed.contains(skill.slug) && _state->skill_registry) {
                _state->skill_registry->install(skill);
            }
        }

        SkillRegistry::Snapshot _snapshot() const
        {
            return _state->skill_registry ? _state->skill_registry->snapshot()
                                          : SkillRegistry::Snapshot { };
        }

        void _sync_results(const SkillRegistry::Snapshot& snap)
        {
            // Only adopt results for the query currently in the box: an
            // in-flight or superseded search must not repopulate the list.
            if (snap.query != _query) {
                return;
            }
            if (snap.results == _results && _installed == _marked) {
                return;
            }
            _results         = snap.results;
            _marked          = _installed;
            _search_selected = std::clamp(_search_selected, 0,
                std::max(0, static_cast<int>(_results.size()) - 1));
        }

        std::set<std::string> _installed_names() const
        {
            std::set<std::string> names;
            for (const Skill& skill : _state->environment->skills()) {
                names.insert(skill.name);
            }
            return names;
        }

        std::shared_ptr<ApplicationState> _state;
        Signal<>::Subscription _subscription;

        std::vector<std::string> _tab_names;
        Component _tabs;
        int _tab = 0;

        std::vector<SkillsModal::Entry> _entries;
        Component _rows_container;
        std::vector<Component> _row_buttons;
        std::map<int, bool> _confirm;
        int _row_selected    = 0;
        int _policy_open_row = -1;
        int _policy_cursor   = 1;

        Component _search_input;
        std::string _query;
        int _query_cursor = 0;
        std::vector<RemoteSkill> _results;
        std::set<std::string> _installed;
        std::set<std::string> _marked;
        std::string _searched;
        int _search_selected = 0;
        std::chrono::steady_clock::time_point _search_due { };
    };

} // namespace

ftxui::Component make_skills(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SkillsView>(state);
}

} // namespace imza
