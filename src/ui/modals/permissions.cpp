#include "app/flows.h"
#include "common/modal.h"
#include "common/util.h"
#include "permissions/shell_analysis.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace imza {

namespace {

    using namespace ftxui;

    constexpr int FORM_LABEL = 9;
    constexpr int COL_GRANT  = 34;
    constexpr int COL_KIND   = 15;

    std::string command_label(const ShellCommandGrant& command)
    {
        return command.subcommand ? command.program + " " + *command.subcommand
                                  : command.program;
    }

    std::string command_kind(const ShellCommandGrant& command)
    {
        return command.subcommand.has_value() ? "one subcommand"
                                              : "whole program";
    }

    class PermissionsView : public ComponentBase {
    public:
        explicit PermissionsView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
        {
            // The tab bar is rendered directly, so it has no parent to
            // decide focus and would otherwise always report focused,
            // inverting the selected tab. Style it without the inverted
            // focus state; the animated underline is unaffected.
            MenuOption tab_option = MenuOption::HorizontalAnimated();
            tab_option.entries_option.transform = [](const EntryState& s) {
                Element e = text(s.label);
                return s.active ? e | bold : e | dim;
            };
            _tab_names = { "Directories", "Commands" };
            _tabs      = Menu(&_tab_names, &_tab, std::move(tab_option));

            _dir_input = Input(field_option(
                &_dir_buf, &_dir_cursor, "path, e.g. ~/shared-assets or docs",
                [this] { _dir_error.clear(); }, [this] { _add_directory(); }));
            _cmd_input = Input(field_option(
                &_cmd_buf, &_cmd_cursor, "command, e.g. git push",
                [this] { _cmd_error.clear(); }, [this] { _add_command(); }));
            _add_dir_button
                = action_button("Add", [this] { _add_directory(); });
            _add_cmd_button = action_button("Add", [this] { _add_command(); });
            // A real component tree, so TakeFocus establishes one honest
            // focus chain: with detached components every one reports
            // Focused(), and the tab menu's focus requirement steals the
            // terminal cursor from the input.
            _content = Container::Vertical({ });
            Add(_content);
            Add(_tabs);
            _refresh_entries();
            _rebuild();
        }

        // The pane holds the active tab's rows and add form. Rebuilt in
        // place: DetachAllChildren drops stale rows, re-Add attaches the
        // current components under the real focus chain.
        void _rebuild_pane()
        {
            _content->DetachAllChildren();
            _content->Add(_tab == 0 ? _dir_rows : _cmd_rows);
            _content->Add(_tab == 0 ? _dir_input : _cmd_input);
            _content->Add(_tab == 0 ? _add_dir_button : _add_cmd_button);
        }

        Element OnRender() override
        {
            _refresh_entries();

            Elements rows = modal_header("Permissions");
            rows.push_back(_tabs->Render());
            rows.push_back(separatorEmpty());
            rows.push_back(
                _tab == 0 ? _render_directories() : _render_commands());
            rows.push_back(separatorEmpty());
            rows.push_back(hint_bar(
                "Tab switch · ↑↓ rows/add form · DEL remove · Enter add · "
                "Esc close"));
            return vbox(std::move(rows)) | xflex;
        }

        // The tab bar is rendered directly rather than attached as a
        // child, so its animation frames (the animated underline) must be
        // forwarded by hand or they never advance.
        void OnAnimation(animation::Params& params) override
        {
            if (_tabs) {
                _tabs->OnAnimation(params);
            }
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::Escape) {
                imza::close_modal(*_state);
                return true;
            }
            if (event == Event::Tab || event == Event::TabReverse) {
                _tab = _tab == 0 ? 1 : 0;
                _dir_confirm.clear();
                _cmd_confirm.clear();
                _dir_in_add = _cmd_in_add = false;
                _rebuild();
                _refocus();
                return true;
            }
            return _tab == 0 ? _handle_directories(event)
                             : _handle_commands(event);
        }

    private:
        static bool _any_confirmed(const std::map<int, bool>& confirm)
        {
            return std::ranges::any_of(
                confirm, [](const auto& entry) { return entry.second; });
        }

        // Picks up grants changed outside the modal (another Imza process
        // saving the config) without discarding in-progress input.
        void _refresh_entries()
        {
            const auto snapshot = _state->config->snapshot();
            if (snapshot->allow.directories == _dirs
                && snapshot->allow.commands == _commands) {
                return;
            }
            _dirs         = snapshot->allow.directories;
            _commands     = snapshot->allow.commands;
            _dir_selected = std::clamp(_dir_selected, 0,
                std::max(0, static_cast<int>(_dirs.size()) - 1));
            _cmd_selected = std::clamp(_cmd_selected, 0,
                std::max(0, static_cast<int>(_commands.size()) - 1));
            _dir_confirm.clear();
            _cmd_confirm.clear();
            _rebuild();
        }

        void _row_move(int delta)
        {
            auto& selected = _tab == 0 ? _dir_selected : _cmd_selected;
            const int count
                = static_cast<int>(_tab == 0 ? _dirs.size() : _commands.size());
            auto& in_add = _tab == 0 ? _dir_in_add : _cmd_in_add;
            // Past the last row the cursor enters the add form, matching
            // the MCP modal's flow; ArrowUp from the form returns.
            if (delta > 0 && (in_add || selected >= count - 1)) {
                in_add = true;
                (_tab == 0 ? _dir_input : _cmd_input)->TakeFocus();
                return;
            }
            if (delta < 0 && in_add) {
                in_add = false;
                if (count == 0) {
                    return;
                }
                selected = count - 1;
                _refocus();
                return;
            }
            if (count == 0) {
                in_add = true;
                (_tab == 0 ? _dir_input : _cmd_input)->TakeFocus();
                return;
            }
            selected = std::clamp(selected + delta, 0, count - 1);
            _refocus();
        }

        void _refocus()
        {
            auto& buttons = _tab == 0 ? _dir_buttons : _cmd_buttons;
            if (buttons.empty()) {
                (_tab == 0 ? _dir_in_add : _cmd_in_add) = true;
                (_tab == 0 ? _dir_input : _cmd_input)->TakeFocus();
                return;
            }
            buttons[static_cast<std::size_t>(
                        std::min(_tab == 0 ? _dir_selected : _cmd_selected,
                            static_cast<int>(buttons.size()) - 1))]
                ->TakeFocus();
        }

        void _rebuild()
        {
            _dir_buttons.clear();
            Components dir_rows;
            for (int i = 0; i < static_cast<int>(_dirs.size()); ++i) {
                dir_rows.push_back(_make_row(true, i));
            }
            _dir_rows = Container::Vertical(std::move(dir_rows));

            _cmd_buttons.clear();
            Components cmd_rows;
            for (int i = 0; i < static_cast<int>(_commands.size()); ++i) {
                cmd_rows.push_back(_make_row(false, i));
            }
            _cmd_rows = Container::Vertical(std::move(cmd_rows));
            _rebuild_pane();
        }

        Component _make_row(bool is_dir, int index)
        {
            std::map<int, bool>& confirm = is_dir ? _dir_confirm : _cmd_confirm;
            const bool is_confirm
                = confirm.count(index) != 0 && confirm.at(index);

            Components controls;
            if (is_confirm) {
                controls.push_back(action_button("Yes", [this, is_dir, index] {
                    if (is_dir) {
                        _confirm_remove_dir(index);
                    } else {
                        _confirm_remove_cmd(index);
                    }
                }));
                controls.push_back(action_button("No", [this, is_dir, index] {
                    if (is_dir) {
                        _dir_confirm.erase(index);
                    } else {
                        _cmd_confirm.erase(index);
                    }
                    _rebuild();
                    _refocus();
                }));
            } else {
                controls.push_back(
                    action_button("Remove", [this, is_dir, index] {
                        if (is_dir) {
                            _dir_confirm[index] = true;
                        } else {
                            _cmd_confirm[index] = true;
                        }
                        _rebuild();
                        _refocus();
                    }));
            }
            Component row = Container::Horizontal(std::move(controls));
            row->SetActiveChild(row);
            (is_dir ? _dir_buttons : _cmd_buttons).push_back(row);
            return Renderer(row, [this, row, index, is_confirm, is_dir] {
                Element line = hbox({ _row_label(is_dir, index), filler(),
                    row->Render() | xflex });
                const bool selected
                    = index == (is_dir ? _dir_selected : _cmd_selected);
                if (is_confirm || selected) {
                    line |= bgcolor(PANEL_COLOR_FOCUS);
                }
                return line;
            });
        }

        Element _row_label(bool is_dir, int index)
        {
            if (is_dir) {
                if (index < 0 || index >= static_cast<int>(_dirs.size())) {
                    return text("");
                }
                return text(fit(_dirs[static_cast<std::size_t>(index)].string(),
                           COL_GRANT))
                    | bold;
            }
            if (index < 0 || index >= static_cast<int>(_commands.size())) {
                return text("");
            }
            return hbox({
                text(fit(
                    command_label(_commands[static_cast<std::size_t>(index)]),
                    COL_GRANT))
                    | bold,
                text("  "),
                text(fit(
                    command_kind(_commands[static_cast<std::size_t>(index)]),
                    COL_KIND))
                    | color(PANEL_FG_DIM),
            });
        }

        void _confirm_remove_dir(int index)
        {
            _dir_confirm.erase(index);
            if (index >= 0 && index < static_cast<int>(_dirs.size())) {
                if (allow_remove_directory(
                        *_state, static_cast<std::size_t>(index))) {
                    _dirs.erase(_dirs.begin() + index);
                }
            }
            _dir_selected = 0;
            _rebuild();
            _refocus();
        }

        void _confirm_remove_cmd(int index)
        {
            _cmd_confirm.erase(index);
            if (index >= 0 && index < static_cast<int>(_commands.size())) {
                if (allow_remove_command(
                        *_state, static_cast<std::size_t>(index))) {
                    _commands.erase(_commands.begin() + index);
                }
            }
            _cmd_selected = 0;
            _rebuild();
            _refocus();
        }

        Element _render_directories()
        {
            Elements rows;
            if (_dirs.empty()) {
                rows.push_back(text("  (none - add one below)") | dim);
            } else {
                rows.push_back(_dir_rows->Render() | yflex);
            }
            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Allow Directory"),
                text("  granted at every start, saved to the config") | dim,
            }));
            rows.push_back(hbox({
                text("  " + std::string(fit("Path", FORM_LABEL))) | bold,
                _dir_input->Render() | xflex,
            }));
            rows.push_back(hbox({
                text(std::string(FORM_LABEL + 2, ' ')),
                _add_dir_button->Render(),
                text("  "),
                _status(_dir_error),
            }));
            return vbox(std::move(rows));
        }

        Element _render_commands()
        {
            Elements rows;
            if (_commands.empty()) {
                rows.push_back(text("  (none - add one below)") | dim);
            } else {
                rows.push_back(_cmd_rows->Render() | yflex);
            }
            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Allow Command"),
                text("  program, or program plus one subcommand") | dim,
            }));
            rows.push_back(hbox({
                text("  " + std::string(fit("Command", FORM_LABEL))) | bold,
                _cmd_input->Render() | xflex,
            }));
            rows.push_back(hbox({
                text(std::string(FORM_LABEL + 2, ' ')),
                _add_cmd_button->Render(),
                text("  "),
                _status(_cmd_error),
            }));
            return vbox(std::move(rows));
        }

        Element _status(const std::string& text_value)
        {
            if (text_value.empty()) {
                return text("");
            }
            return status_text(text_value, false);
        }

        // Arrows move the row cursor; everything else reaches the focused
        // control. Enter and Space activate the focused button, matching
        // button behavior; the row's Left/Right keys choose which control
        // of the row is focused.
        bool _handle_directories(const Event& event)
        {
            if (_any_confirmed(_dir_confirm)) {
                return _dir_rows ? _dir_rows->OnEvent(event) : false;
            }
            if (_dir_in_add) {
                if (event == Event::ArrowUp && !_dirs.empty()) {
                    _row_move(-1);
                    return true;
                }
                return _dir_input->OnEvent(event);
            }
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                _row_move(event == Event::ArrowDown ? 1 : -1);
                return true;
            }
            if (event == Event::Delete && !_dirs.empty()) {
                _dir_confirm[_dir_selected] = true;
                _rebuild();
                _refocus();
                return true;
            }
            return _dir_rows ? _dir_rows->OnEvent(event) : false;
        }

        bool _handle_commands(const Event& event)
        {
            if (_any_confirmed(_cmd_confirm)) {
                return _cmd_rows ? _cmd_rows->OnEvent(event) : false;
            }
            if (_cmd_in_add) {
                if (event == Event::ArrowUp && !_commands.empty()) {
                    _row_move(-1);
                    return true;
                }
                return _cmd_input->OnEvent(event);
            }
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                _row_move(event == Event::ArrowDown ? 1 : -1);
                return true;
            }
            if (event == Event::Delete && !_commands.empty()) {
                _cmd_confirm[_cmd_selected] = true;
                _rebuild();
                _refocus();
                return true;
            }
            return _cmd_rows ? _cmd_rows->OnEvent(event) : false;
        }

        void _add_directory()
        {
            std::string error;
            if (allow_add_directory(*_state, _dir_buf, error)) {
                _dir_buf.clear();
                _dir_cursor = 0;
                _dir_error.clear();
                _dir_in_add = false;
                _dir_selected
                    = static_cast<int>(
                          _state->config->snapshot()->allow.directories.size())
                    - 1;
                _refresh_entries();
                _refocus();
            } else {
                _dir_error = error;
            }
        }

        void _add_command()
        {
            std::string error;
            if (allow_add_command(*_state, _cmd_buf, error)) {
                _cmd_buf.clear();
                _cmd_cursor = 0;
                _cmd_error.clear();
                _cmd_in_add = false;
                _cmd_selected
                    = static_cast<int>(
                          _state->config->snapshot()->allow.commands.size())
                    - 1;
                _refresh_entries();
                _refocus();
            } else {
                _cmd_error = error;
            }
        }

        std::shared_ptr<ApplicationState> _state;

        Component _content;
        std::vector<std::string> _tab_names;
        Component _tabs;
        int _tab = 0;

        std::vector<std::filesystem::path> _dirs;
        std::vector<ShellCommandGrant> _commands;
        Component _dir_rows;
        Component _cmd_rows;
        std::vector<Component> _dir_buttons;
        std::vector<Component> _cmd_buttons;
        std::map<int, bool> _dir_confirm;
        std::map<int, bool> _cmd_confirm;
        int _dir_selected = 0;
        int _cmd_selected = 0;
        bool _dir_in_add  = false;
        bool _cmd_in_add  = false;
        std::string _dir_error;
        std::string _cmd_error;

        Component _dir_input;
        std::string _dir_buf;
        int _dir_cursor = 0;
        Component _cmd_input;
        std::string _cmd_buf;
        int _cmd_cursor = 0;
        Component _add_dir_button;
        Component _add_cmd_button;
    };

} // namespace

ftxui::Component make_permissions(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<PermissionsView>(state);
}

} // namespace imza
