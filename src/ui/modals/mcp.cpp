#include "app/flows.h"
#include "common/modal.h"
#include "common/types.h"
#include "common/util.h"
#include "network/web.h"
#include "platform/config.h"
#include "tools/mcp_catalog.h"
#include "tools/mcp_manager.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <string>

namespace imza {

namespace {

    using namespace ftxui;

    constexpr int FORM_LABEL                    = 10;
    constexpr int COL_NAME                      = 24;
    constexpr int COL_STATE                     = 2;
    constexpr int COL_DETAIL                    = 20;
    constexpr std::string_view CUSTOM_SERVER_ID = "custom";

    // Server ids are Lua-callable paths: lowercase, [a-z0-9_-] only.
    std::string sanitize_id(const std::string& raw)
    {
        std::string out;
        for (const char c : raw) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                out += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            } else if (!out.empty() && out.back() != '-') {
                out += '-';
            }
        }
        while (!out.empty() && out.back() == '-') {
            out.pop_back();
        }
        return out;
    }

    Element state_glyph(McpServerState state)
    {
        switch (state) {
        case McpServerState::CONNECTED: return text("✓") | color(HL_GREEN);
        case McpServerState::FAILED: return text("✗") | color(HL_RED);
        case McpServerState::CONNECTING: return text("⟳") | color(PANEL_FG_DIM);
        default: return text("○") | color(PANEL_FG_DIM);
        }
    }

    std::string state_text(const McpServerSnapshot& server)
    {
        switch (server.state) {
        case McpServerState::CONNECTED:
            return std::to_string(server.tool_count) + " tools";
        case McpServerState::CONNECTING: return "connecting…";
        case McpServerState::FAILED:
            return server.detail.empty() ? std::string("failed")
                                         : server.detail;
        case McpServerState::DISABLED: return "disabled";
        case McpServerState::OFFLINE: return "offline";
        }
        return "";
    }

    std::vector<std::string> split_ws(const std::string& text)
    {
        std::vector<std::string> out;
        std::string current;
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                if (!current.empty()) {
                    out.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            out.push_back(std::move(current));
        }
        return out;
    }

    // Parses "KEY=VALUE KEY2=VALUE2"; false on a malformed token.
    bool parse_env_pairs(
        const std::string& text, std::map<std::string, std::string>& out)
    {
        for (const std::string& token : split_ws(text)) {
            const std::size_t eq = token.find('=');
            if (eq == std::string::npos || eq == 0) {
                return false;
            }
            out[token.substr(0, eq)] = token.substr(eq + 1);
        }
        return true;
    }

    // Host part of an https URL, sanitized into a server id.
    std::string id_from_url(const std::string& url)
    {
        std::string host  = url;
        const auto scheme = host.find("://");
        if (scheme != std::string::npos) {
            host = host.substr(scheme + 3);
        }
        const auto slash = host.find('/');
        if (slash != std::string::npos) {
            host = host.substr(0, slash);
        }
        const auto colon = host.find(':');
        if (colon != std::string::npos) {
            host = host.substr(0, colon);
        }
        const auto dot = host.rfind('.');
        if (dot != std::string::npos
            && host.substr(0, dot).find('.') != std::string::npos) {
            host = host.substr(0, dot); // keep a domain, drop only the tld
        }
        return sanitize_id(host);
    }

    class McpView : public ComponentBase {
    public:
        explicit McpView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
            , _session(_state->session)
            , _catalog(load_mcp_catalog())
        {
            if (_state->mcp != nullptr) {
                // Manager workers announce on their thread; bump the modal
                // serial on the main thread so the rows refresh.
                _subscription = _state->mcp->subscribe([state = _state] {
                    state->post(
                        [state] { state->session->bump_modal_serial(); });
                });
            }
        }

        Element OnRender() override
        {
            _maybe_rebuild();
            return _render();
        }

        bool OnEvent(Event event) override
        {
            if (_picker_open) {
                if (event == Event::ArrowDown || event == Event::ArrowUp) {
                    move_list_cursor(event, _picker_selected,
                        static_cast<int>(_picker_ids.size()));
                    return true;
                }
                if (event == Event::Escape) {
                    _close_picker();
                    return true;
                }
                if (event == Event::Return) {
                    _commit_picker();
                    return true;
                }
                return _container ? _container->OnEvent(event) : false;
            }

            if (_confirming()) {
                if (event == Event::Character('y')) {
                    _confirm_remove(_row_selected);
                    return true;
                }
                if (event == Event::Character('n') || event == Event::Escape) {
                    _confirm.clear();
                    _maybe_rebuild();
                    return true;
                }
                return _container ? _container->OnEvent(event) : false;
            }

            const bool focus_in_add
                = _add_container && _add_container->Focused();
            if (!_in_add && !focus_in_add) {
                if (event == Event::ArrowDown) {
                    _row_move(1);
                    return true;
                }
                if (event == Event::ArrowUp) {
                    _row_move(-1);
                    return true;
                }
                if (event == Event::Delete) {
                    if (_row_selected >= 0
                        && _row_selected
                            < static_cast<int>(_servers().size())) {
                        _confirm[_row_selected] = true;
                        _maybe_rebuild();
                    }
                    return true;
                }
            } else if (event == Event::ArrowUp && _picker_input
                && _picker_input->Focused()) {
                _in_add        = false;
                const auto all = _servers();
                _row_selected
                    = all.empty() ? 0 : static_cast<int>(all.size()) - 1;
                if (_row_selected < static_cast<int>(_row_buttons.size())) {
                    _row_buttons[static_cast<std::size_t>(_row_selected)]
                        ->TakeFocus();
                }
                return true;
            }
            return _container ? _container->OnEvent(event) : false;
        }

    private:
        std::vector<McpServerSnapshot> _servers()
        {
            return _state->mcp ? _state->mcp->snapshot()
                               : std::vector<McpServerSnapshot> { };
        }

        const McpCatalogEntry* _selected_entry() const
        {
            if (_selected.empty() || _selected == CUSTOM_SERVER_ID) {
                return nullptr;
            }
            for (const McpCatalogEntry& entry : _catalog) {
                if (entry.id == _selected) {
                    return &entry;
                }
            }
            return nullptr;
        }

        bool _is_custom() const { return _selected == CUSTOM_SERVER_ID; }
        bool _is_oauth() const
        {
            const McpCatalogEntry* entry = _selected_entry();
            return entry != nullptr && entry->auth_kind == "oauth";
        }
        bool _needs_token() const
        {
            if (_is_custom()) {
                return true; // optional for custom, field still shown
            }
            const McpCatalogEntry* entry = _selected_entry();
            return entry != nullptr && entry->auth_kind == "token";
        }

        std::string _selected_name() const
        {
            if (_is_custom()) {
                return "Custom server";
            }
            const McpCatalogEntry* entry = _selected_entry();
            return entry != nullptr ? entry->label : std::string { };
        }

        std::string _current_serial()
        {
            return std::to_string(_session->modal_serial()) + "/"
                + std::to_string(_servers().size()) + "/" + _selected + "/"
                + std::to_string(_confirming()) + "/" + std::to_string(_stdio)
                + "/" + std::to_string(_in_add);
        }

        bool _confirming() const
        {
            return std::ranges::any_of(
                _confirm, [](const auto& entry) { return entry.second; });
        }

        void _maybe_rebuild()
        {
            const std::string serial = _current_serial();
            if (_built && serial == _serial) {
                return;
            }
            _built  = true;
            _serial = serial;
            _rebuild();
        }

        void _row_move(int delta)
        {
            const auto all         = _servers();
            const bool at_last_row = delta > 0
                && _row_selected >= static_cast<int>(all.size()) - 1;
            if (all.empty() || at_last_row) {
                _in_add = true;
                if (_picker_input) {
                    _picker_input->TakeFocus();
                }
                return;
            }
            _row_selected = std::clamp(
                _row_selected + delta, 0, static_cast<int>(all.size()) - 1);
            if (_row_selected < static_cast<int>(_row_buttons.size())) {
                _row_buttons[static_cast<std::size_t>(_row_selected)]
                    ->TakeFocus();
            }
        }

        void _refill_picker()
        {
            const std::string needle = to_lower(trim(_picker_buf));
            _picker_labels.clear();
            _picker_ids.clear();
            _picker_selected = 0;
            const auto add   = [&](std::string id, std::string label) {
                if (!needle.empty()
                    && to_lower(label).find(needle) == std::string::npos
                    && id.find(needle) == std::string::npos) {
                    return;
                }
                _picker_ids.push_back(std::move(id));
                _picker_labels.push_back(std::move(label));
            };
            add(std::string(CUSTOM_SERVER_ID), "Custom server…");
            for (const McpCatalogEntry& entry : _catalog) {
                std::string label = entry.label;
                if (entry.auth_kind == "oauth") {
                    label += "  (sign-in)";
                } else if (entry.auth_kind == "token") {
                    label += "  (key)";
                }
                add(entry.id, std::move(label));
            }
        }

        void _commit_picker()
        {
            if (_picker_ids.empty()
                || _picker_selected >= static_cast<int>(_picker_ids.size())) {
                _close_picker();
                return;
            }
            _selected = _picker_ids[static_cast<std::size_t>(_picker_selected)];
            _picker_buf    = _selected_name();
            _picker_cursor = static_cast<int>(_picker_buf.size());
            _picker_open   = false;
            _row_error.clear();
            if (!_is_custom()) {
                _stdio = false;
            }
            _maybe_rebuild();
            if (_stdio && _command_input) {
                _command_input->TakeFocus();
            } else if (_url_input) {
                _url_input->TakeFocus();
            } else if (_token_input) {
                _token_input->TakeFocus();
            } else if (_label_input) {
                _label_input->TakeFocus();
            }
        }

        void _close_picker()
        {
            _picker_buf    = _selected_name();
            _picker_cursor = static_cast<int>(_picker_buf.size());
            _picker_open   = false;
        }

        void _rebuild()
        {
            _refill_picker();
            _picker_input = Input(field_option(
                &_picker_buf, &_picker_cursor, "type to search servers",
                [this] {
                    _row_error.clear();
                    if (!_picker_open) {
                        _picker_open = true;
                    }
                    _refill_picker();
                },
                [this] {
                    if (_picker_open) {
                        _commit_picker();
                    }
                }));

            const bool custom = _is_custom();
            const bool oauth  = _is_oauth();
            const bool stdio  = custom && _stdio;

            _label_input = Input(field_option(&_label_buf, &_label_cursor,
                custom ? "label (optional)" : "label (optional)",
                [this] { _row_error.clear(); }));

            if (custom && !stdio) {
                _url_input     = Input(field_option(&_url_buf, &_url_cursor,
                    "URL, e.g. https://mcp.example.com/mcp",
                    [this] { _row_error.clear(); }));
                _timeout_input = Input(field_option(&_timeout_buf,
                    &_timeout_cursor, "timeout seconds (optional)",
                    [this] { _row_error.clear(); }));
            } else {
                _url_input.reset();
                _timeout_input.reset();
            }
            if (stdio) {
                _command_input
                    = Input(field_option(&_command_buf, &_command_cursor,
                        "command, e.g. npx", [this] { _row_error.clear(); }));
                _args_input    = Input(field_option(&_args_buf, &_args_cursor,
                    "arguments (space-separated, optional)",
                    [this] { _row_error.clear(); }));
                _env_input     = Input(field_option(&_env_buf, &_env_cursor,
                    "env KEY=VALUE pairs (space-separated, optional)",
                    [this] { _row_error.clear(); }));
                _workdir_input = Input(field_option(&_workdir_buf,
                    &_workdir_cursor, "working directory (optional)",
                    [this] { _row_error.clear(); }));
            } else {
                _command_input.reset();
                _args_input.reset();
                _env_input.reset();
                _workdir_input.reset();
            }
            if (_needs_token() && !oauth && !stdio) {
                _token_input
                    = Input(password_option(&_token_buf, &_token_cursor,
                        custom ? "bearer token (optional)" : "API key",
                        [this] { _row_error.clear(); }));
            } else {
                _token_input.reset();
            }
            if (!oauth) {
                _add_button = action_button("Add", [this] { _add_server(); });
            } else {
                _add_button.reset();
            }
            if (custom) {
                // The serial includes _stdio, so the next render rebuilds
                // the form.
                _transport_button = inline_link_button(
                    [this] {
                        Element http  = text("HTTP") | (_stdio ? dim : bold);
                        Element stdio = text("stdio") | (_stdio ? bold : dim);
                        return hbox({
                            text(choice_marker(false, !_stdio)),
                            std::move(http),
                            text("   "),
                            text(choice_marker(false, _stdio)),
                            std::move(stdio),
                        });
                    },
                    [this] {
                        _stdio             = !_stdio;
                        _in_add            = true;
                        _refocus_transport = true;
                        _row_error.clear();
                    });
            } else {
                _transport_button.reset();
            }

            Components rows;
            const auto all = _servers();
            _row_buttons.clear();
            for (int i = 0; i < static_cast<int>(all.size()); ++i) {
                rows.push_back(_make_row(i));
            }
            _rows_container = Container::Vertical(std::move(rows));

            Components add_parts;
            add_parts.push_back(_picker_input);
            if (custom) {
                add_parts.push_back(_transport_button);
            }
            if (_needs_token() && !oauth && !stdio) {
                add_parts.push_back(_token_input);
            }
            if (stdio) {
                add_parts.push_back(_command_input);
                add_parts.push_back(_args_input);
                add_parts.push_back(_env_input);
                add_parts.push_back(_workdir_input);
            } else if (custom) {
                add_parts.push_back(_url_input);
                add_parts.push_back(_timeout_input);
            }
            add_parts.push_back(_label_input);
            if (!oauth) {
                add_parts.push_back(_add_button);
            }
            _add_container = Container::Vertical(std::move(add_parts));

            _container
                = Container::Vertical({ _rows_container, _add_container });

            if (_refocus_transport && _transport_button) {
                // The toggle rebuilds the form; keep focus on it instead of
                // dropping back to the rows at the top.
                _refocus_transport = false;
                _transport_button->TakeFocus();
            } else if (_in_add) {
                if (_picker_input) {
                    _picker_input->TakeFocus();
                }
            } else if (!_row_buttons.empty()) {
                _row_buttons[static_cast<std::size_t>(std::min(_row_selected,
                                 static_cast<int>(_row_buttons.size()) - 1))]
                    ->TakeFocus();
            }
        }

        Component _make_row(int index)
        {
            const bool is_confirm
                = _confirm.count(index) != 0 && _confirm.at(index);

            Component label = Renderer([this, index] {
                const auto all = _servers();
                if (index < 0 || index >= static_cast<int>(all.size())) {
                    return text("");
                }
                const McpServerSnapshot& server
                    = all[static_cast<std::size_t>(index)];
                const bool highlighted = index == _row_selected && !_in_add;
                std::string name       = server.id;
                if (!server.label.empty() && server.label != server.id) {
                    name += " (" + server.label + ")";
                }
                Element name_el = text(fit(name, COL_NAME));
                if (highlighted) {
                    name_el = std::move(name_el) | bold;
                }
                Element state_el
                    = state_glyph(server.state) | size(WIDTH, EQUAL, COL_STATE);
                Element detail;
                if (server.state == McpServerState::FAILED) {
                    detail = status_text(
                        fit(state_text(server), COL_DETAIL), false);
                } else {
                    detail = text(fit(state_text(server), COL_DETAIL)) | dim;
                }
                return hbox({ std::move(name_el), std::move(state_el),
                    text(" "), std::move(detail) });
            });

            Component right;
            if (is_confirm) {
                Component yes = action_button(
                    "Yes", [this, index] { _confirm_remove(index); });
                Component no = action_button("No", [this, index] {
                    _confirm.erase(index);
                    _maybe_rebuild();
                });
                right        = Container::Horizontal(
                    { yes, Renderer([] { return text(" "); }), no });
            } else {
                const McpServerSnapshot server
                    = _servers()[static_cast<std::size_t>(index)];
                Components buttons;
                if (server.state == McpServerState::CONNECTED
                    || server.state == McpServerState::FAILED) {
                    buttons.push_back(
                        action_button("Disconnect", [this, id = server.id] {
                            if (_state->mcp) {
                                _state->mcp->disconnect(id);
                            }
                        }));
                } else if (server.state != McpServerState::CONNECTING
                    && server.state != McpServerState::DISABLED) {
                    buttons.push_back(
                        action_button("Connect", [this, id = server.id] {
                            if (_state->mcp) {
                                _state->mcp->connect(id);
                            }
                        }));
                }
                buttons.push_back(action_button(
                    server.state == McpServerState::DISABLED ? "Enable"
                                                             : "Disable",
                    [this, id = server.id,
                        enable = server.state == McpServerState::DISABLED] {
                        imza::mcp_set_server_enabled(*_state, id, enable);
                    }));
                buttons.push_back(action_button("Remove", [this, index] {
                    _confirm[index] = true;
                    _maybe_rebuild();
                }));
                right = Container::Horizontal(std::move(buttons));
            }

            Component row = Container::Horizontal({ label, right });
            row->SetActiveChild(right);
            _row_buttons.push_back(right);
            return Renderer(row, [row, index, is_confirm, this] {
                Element e = row->Render() | xflex;
                if (is_confirm || (index == _row_selected && !_in_add)) {
                    e |= bgcolor(PANEL_COLOR_FOCUS);
                }
                return e;
            });
        }

        void _confirm_remove(int index)
        {
            _confirm.erase(index);
            const auto all = _servers();
            if (index < 0 || index >= static_cast<int>(all.size())) {
                return;
            }
            const std::string id = all[static_cast<std::size_t>(index)].id;
            if (imza::mcp_remove_server(*_state, id)) {
                _row_error.clear();
            } else {
                _row_error = "Could not save the configuration.";
            }
            _row_selected = 0;
            _maybe_rebuild();
        }

        void _add_server()
        {
            McpServerConfig server;
            const McpCatalogEntry* entry = _selected_entry();
            if (entry != nullptr) {
                server.id         = entry->id;
                server.catalog_id = entry->id;
                server.label      = std::string(trim(_label_buf));
            } else if (_is_custom() && _stdio) {
                const std::string command = std::string(trim(_command_buf));
                if (command.empty()) {
                    _row_error = "Enter a command to run.";
                    return;
                }
                server.type    = "stdio";
                server.command = command;
                server.args    = split_ws(_args_buf);
                if (!parse_env_pairs(_env_buf, server.env)) {
                    _row_error = "Env must be KEY=VALUE pairs.";
                    return;
                }
                server.working_directory = std::string(trim(_workdir_buf));
                server.label             = std::string(trim(_label_buf));
                server.id = sanitize_id(std::string(trim(_label_buf)));
                if (server.id.empty()) {
                    server.id = sanitize_id(command);
                }
                if (server.id.empty()) {
                    _row_error = "Enter a label for a custom server.";
                    return;
                }
            } else if (_is_custom()) {
                std::string url;
                if (normalize_web_url(std::string(trim(_url_buf)), url)
                    != Status::OK) {
                    _row_error = "Enter an http(s) URL.";
                    return;
                }
                server.url   = url;
                server.label = std::string(trim(_label_buf));
                server.id    = sanitize_id(std::string(trim(_label_buf)));
                if (server.id.empty()) {
                    server.id = id_from_url(url);
                }
                if (server.id.empty()) {
                    _row_error = "Enter a label for a custom server.";
                    return;
                }
                const std::string timeout_text(std::string(trim(_timeout_buf)));
                if (!timeout_text.empty()) {
                    long timeout_secs    = 0;
                    const auto [ptr, ec] = std::from_chars(timeout_text.data(),
                        timeout_text.data() + timeout_text.size(),
                        timeout_secs);
                    if (ec != std::error_code { } || timeout_secs < 0) {
                        _row_error = "Timeout must be a number of seconds.";
                        return;
                    }
                    server.timeout_secs = timeout_secs;
                }
            } else {
                _row_error = "Pick a server first.";
                return;
            }
            server.bearer_token = std::string(trim(_token_buf));
            server.enabled      = true;

            for (const auto& existing : _servers()) {
                if (existing.id == server.id) {
                    _row_error = "Id already in use.";
                    return;
                }
            }

            imza::mcp_add_server(*_state, server);
            _row_error.clear();
            _selected.clear();
            _label_buf.clear();
            _url_buf.clear();
            _token_buf.clear();
            _timeout_buf.clear();
            _command_buf.clear();
            _args_buf.clear();
            _env_buf.clear();
            _workdir_buf.clear();
            _stdio = false;
            _close_picker();
            _maybe_rebuild();
        }

        Element _picker_area()
        {
            const std::string pad(FORM_LABEL + 2, ' ');
            Elements rows;
            rows.push_back(_picker_input->Render() | xflex);
            if (_picker_open) {
                rows.push_back(
                    text(pad + std::to_string(_picker_ids.size()) + " server"
                        + (_picker_ids.size() == 1 ? "" : "s"))
                    | dim);
                if (_picker_ids.empty()) {
                    rows.push_back(text(pad + "no matching servers") | dim);
                } else {
                    for (int i = 0; i < static_cast<int>(_picker_labels.size());
                        ++i) {
                        const bool selected = i == _picker_selected;
                        Element e = text(pad + (selected ? "› " : "  ")
                            + _picker_labels[static_cast<std::size_t>(i)]);
                        if (selected) {
                            e = std::move(e) | bold | color(PANEL_FG);
                        } else {
                            e = std::move(e) | color(PANEL_FG_DIM);
                        }
                        rows.push_back(std::move(e));
                    }
                }
            }
            return vbox(std::move(rows));
        }

        Element _form_row(const std::string& label, const Component& input)
        {
            return hbox({
                text("  " + std::string(fit(label, FORM_LABEL))) | bold,
                input->Render() | xflex,
            });
        }

        Element _status_line_element()
        {
            if (!_row_error.empty()) {
                return status_text(_row_error, false);
            }
            return text("");
        }

        Element _render()
        {
            Elements rows = modal_header("MCP Servers");

            const auto servers = _servers();
            if (servers.empty()) {
                rows.push_back(text("  (none - add one below)") | dim);
            }
            if (_rows_container != nullptr) {
                rows.push_back(_rows_container->Render() | yflex);
            }

            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Add Server"),
                text("  pick a server, paste a token, then add") | dim,
            }));
            if (_add_container != nullptr) {
                rows.push_back(hbox({
                    text("  " + std::string(fit("Server", FORM_LABEL))) | bold,
                    _picker_area() | xflex,
                }));
                if (_is_oauth()) {
                    const McpCatalogEntry* entry = _selected_entry();
                    rows.push_back(
                        hbox({ text("  "),
                            text("browser sign-in for "
                                + (entry ? entry->label
                                         : std::string("this server"))
                                + " is not supported yet")
                                | dim })
                        | xflex);
                } else {
                    if (_transport_button) {
                        rows.push_back(
                            _form_row("Transport", _transport_button));
                    }
                    if (_token_input) {
                        rows.push_back(_form_row(
                            _is_custom() ? "Token" : "API Key", _token_input));
                    }
                    if (_url_input) {
                        rows.push_back(_form_row("URL", _url_input));
                    }
                    if (_timeout_input) {
                        rows.push_back(_form_row("Timeout", _timeout_input));
                    }
                    if (_command_input) {
                        rows.push_back(_form_row("Command", _command_input));
                    }
                    if (_args_input) {
                        rows.push_back(_form_row("Args", _args_input));
                    }
                    if (_env_input) {
                        rows.push_back(_form_row("Env", _env_input));
                    }
                    if (_workdir_input) {
                        rows.push_back(_form_row("Work dir", _workdir_input));
                    }
                    rows.push_back(_form_row("Label", _label_input));
                    rows.push_back(hbox({
                        text(std::string(FORM_LABEL + 2, ' ')),
                        _add_button ? _add_button->Render() : text(""),
                        text("  "),
                        _status_line_element(),
                    }));
                }
            }
            rows.push_back(separatorEmpty());
            std::string hint = "↑↓ navigate · Enter/DEL remove · Esc close";
            if (!_confirm.empty()) {
                hint = "y confirm · n cancel";
            }
            rows.push_back(hint_bar(hint));
            return vbox(std::move(rows)) | xflex;
        }

        std::shared_ptr<ApplicationState> _state;
        std::shared_ptr<Session> _session;
        Signal<>::Subscription _subscription;
        std::vector<McpCatalogEntry> _catalog;

        Component _container;
        Component _rows_container;
        Component _add_container;
        std::vector<Component> _row_buttons;
        std::map<int, bool> _confirm;
        std::string _row_error;
        std::string _serial;
        bool _built = false;

        bool _picker_open = false;
        std::string _selected;
        std::string _picker_buf;
        int _picker_cursor = 0;
        std::vector<std::string> _picker_ids;
        std::vector<std::string> _picker_labels;
        int _picker_selected = 0;
        Component _picker_input;

        int _row_selected       = 0;
        bool _in_add            = false;
        bool _refocus_transport = false;

        Component _label_input;
        Component _token_input;
        Component _url_input;
        Component _timeout_input;
        Component _add_button;
        std::string _label_buf;
        int _label_cursor = 0;
        std::string _token_buf;
        int _token_cursor = 0;
        std::string _url_buf;
        int _url_cursor = 0;
        std::string _timeout_buf;
        int _timeout_cursor = 0;

        bool _stdio = false; // custom-server transport choice
        Component _transport_button;
        Component _command_input;
        Component _args_input;
        Component _env_input;
        Component _workdir_input;
        std::string _command_buf;
        int _command_cursor = 0;
        std::string _args_buf;
        int _args_cursor = 0;
        std::string _env_buf;
        int _env_cursor = 0;
        std::string _workdir_buf;
        int _workdir_cursor = 0;
    };

} // namespace

ftxui::Component make_mcp(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<McpView>(std::move(state));
}

} // namespace imza
