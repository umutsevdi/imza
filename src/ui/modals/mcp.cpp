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

    constexpr int FORM_LABEL                    = 9;
    constexpr int PICKER_ROWS                   = 8;
    constexpr std::string_view CUSTOM_SERVER_ID = "custom";

    Element status_element(const std::string& text_value, bool ok)
    {
        return text(text_value) | (ok ? color(HL_GREEN) : color(HL_RED));
    }

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
            if (event == Event::Escape) {
                imza::close_modal(*_state);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                if (move_list_cursor(event, _picker_selected,
                        static_cast<int>(_picker_ids.size()))) {
                    _clamp_picker_window();
                    return true;
                }
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

        std::string _current_serial()
        {
            return std::to_string(_session->modal_serial()) + "/"
                + std::to_string(_servers().size()) + "/" + _selected + "/"
                + std::to_string(_confirming());
        }

        bool _confirming() const
        {
            for (const auto& entry : _confirm) {
                if (entry.second) {
                    return true;
                }
            }
            return false;
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

        void _refill_picker()
        {
            const std::string needle = to_lower(trim(_picker_buf));
            _picker_labels.clear();
            _picker_ids.clear();
            _picker_selected = 0;
            _picker_begin    = 0;
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
            if (_picker_selected < 0
                || _picker_selected >= static_cast<int>(_picker_ids.size())) {
                _close_picker();
                return;
            }
            _selected = _picker_ids[static_cast<std::size_t>(_picker_selected)];
            _row_error.clear();
            _maybe_rebuild();
            if (_token_input) {
                _token_input->TakeFocus();
            } else if (_url_input) {
                _url_input->TakeFocus();
            } else if (_label_input) {
                _label_input->TakeFocus();
            }
        }

        void _close_picker()
        {
            _picker_buf.clear();
            _picker_cursor = 0;
            _refill_picker();
        }

        // Keeps the selected row inside the visible window.
        void _clamp_picker_window()
        {
            const int count = static_cast<int>(_picker_labels.size());
            if (_picker_selected < _picker_begin) {
                _picker_begin = _picker_selected;
            }
            if (_picker_selected >= _picker_begin + PICKER_ROWS) {
                _picker_begin = _picker_selected - PICKER_ROWS + 1;
            }
            _picker_begin = std::clamp(
                _picker_begin, 0, std::max(0, count - PICKER_ROWS));
        }

        void _rebuild()
        {
            Components rows;
            _row_buttons.clear();
            for (const auto& server : _servers()) {
                rows.push_back(_make_row(server));
            }
            _rows_container = Container::Vertical(std::move(rows));

            // Add-server form: the picker plus only the fields the
            // selection needs (connect's custom-provider pattern).
            _refill_picker();
            _picker_input = Input(field_option(
                &_picker_buf, &_picker_cursor, "type to search servers",
                [this] {
                    _row_error.clear();
                    _refill_picker();
                    _clamp_picker_window();
                },
                [this] { _commit_picker(); }));

            _label_input = Input(field_option(&_label_buf, &_label_cursor,
                _is_custom() ? "label (optional, derived from the URL)"
                             : "label (optional)",
                [this] { _row_error.clear(); }));

            const bool custom = _is_custom();
            const bool oauth  = _is_oauth();
            if (custom) {
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
            if (_needs_token() && !oauth) {
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

            Components add_parts;
            add_parts.push_back(_picker_input);
            if (_needs_token() && !oauth) {
                add_parts.push_back(_token_input);
            }
            if (custom) {
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
        }

        Component _make_row(const McpServerSnapshot& server)
        {
            const int index = static_cast<int>(_row_buttons.size());
            const bool confirming
                = _confirm.count(index) != 0 && _confirm.at(index);

            Component label = Renderer([server] {
                std::string name = server.id;
                if (!server.label.empty() && server.label != server.id) {
                    name += " (" + server.label + ")";
                }
                Element detail;
                if (server.state == McpServerState::FAILED) {
                    detail = status_element(fit(state_text(server), 44), false);
                } else {
                    detail = text(fit(state_text(server), 12)) | dim;
                }
                return hbox({ text(name) | bold, filler(),
                    state_glyph(server.state), text(" "), std::move(detail) });
            });

            Component actions;
            if (confirming) {
                Component yes = action_button(
                    "Yes", [this, index] { _confirm_remove(index); });
                Component no = action_button("No", [this, index] {
                    _confirm.erase(index);
                    _maybe_rebuild();
                });
                actions      = Container::Horizontal(
                    { yes, Renderer([] { return text(" "); }), no });
            } else {
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
                        _toggle_enabled(id, enable);
                    }));
                buttons.push_back(action_button("Remove", [this, index] {
                    _confirm[index] = true;
                    _maybe_rebuild();
                }));
                actions = Container::Horizontal(std::move(buttons));
            }

            _row_buttons.push_back(actions);
            Component row = Container::Horizontal({ label, actions });
            row->SetActiveChild(actions);
            return row;
        }

        void _toggle_enabled(const std::string& id, bool enable)
        {
            Config initial = _state->providers->config();
            Config result;
            const ConfigUpdateResult updated = update_config(
                config_path(), initial,
                [&id, enable](Config& cfg) {
                    const auto found = cfg.mcp_servers.find(id);
                    if (found == cfg.mcp_servers.end()) {
                        return false;
                    }
                    found->second.enabled = enable;
                    return true;
                },
                &result);
            if (updated == ConfigUpdateResult::UPDATED && _state->mcp) {
                _state->mcp->reload(std::move(result.mcp_servers));
            }
        }

        void _confirm_remove(int index)
        {
            _confirm.erase(index);
            auto snapshot = _servers();
            if (index < 0 || index >= static_cast<int>(snapshot.size())) {
                return;
            }
            const std::string id = snapshot[static_cast<std::size_t>(index)].id;
            Config initial       = _state->providers->config();
            Config result;
            const ConfigUpdateResult updated = update_config(
                config_path(), initial,
                [&id](Config& cfg) { return cfg.mcp_servers.erase(id) != 0; },
                &result);
            if (updated != ConfigUpdateResult::UPDATED) {
                _row_error = "Could not save the configuration.";
            } else {
                _row_error.clear();
                if (_state->mcp) {
                    _state->mcp->reload(std::move(result.mcp_servers));
                }
            }
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
            _close_picker();
            _maybe_rebuild();
        }

        Element _picker_area()
        {
            const std::string pad(FORM_LABEL + 2, ' ');
            Elements rows;
            rows.push_back(_picker_input->Render() | xflex);
            const int count = static_cast<int>(_picker_labels.size());
            if (count == 0) {
                rows.push_back(text(pad + "no matching servers") | dim);
                return vbox(std::move(rows));
            }
            _clamp_picker_window();
            const int shown = std::min(count, PICKER_ROWS);
            const int end   = _picker_begin + shown;
            if (count > shown) {
                rows.push_back(
                    text(pad + (_picker_begin > 0 ? "↑ " : "")
                        + std::to_string(count) + " servers"
                        + (end < count ? " · ↓ " + std::to_string(count - end)
                                    + " more"
                                       : ""))
                    | dim);
            }
            for (int i = _picker_begin; i < end; ++i) {
                const bool selected = i == _picker_selected;
                Element e           = text(pad + (selected ? "› " : "  ")
                    + _picker_labels[static_cast<std::size_t>(i)]);
                e                   = std::move(e)
                    | (selected ? color(PANEL_FG) | bold : color(PANEL_FG_DIM));
                rows.push_back(std::move(e));
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

        Element _render()
        {
            Elements rows = modal_header("MCP Servers");

            const auto servers = _servers();
            if (servers.empty()) {
                rows.push_back(text("  (none yet - add one below)") | dim);
            }
            if (_rows_container != nullptr) {
                rows.push_back(_rows_container->Render() | yflex);
            }

            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Add Server"),
                text("  pick a server, or Custom for any endpoint") | dim,
            }));
            if (_add_container != nullptr) {
                rows.push_back(
                    hbox({ text("  " + std::string(fit("Server", FORM_LABEL)))
                            | bold,
                        _picker_area() | xflex }));
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
                    rows.push_back(_form_row("Label", _label_input));
                    rows.push_back(hbox({
                        text("  "),
                        _add_button ? _add_button->Render() : text(""),
                        text("  "),
                        _row_error.empty() ? text("")
                                           : status_element(_row_error, false),
                    }));
                }
            }
            rows.push_back(separatorEmpty());
            std::string hint
                = "type to filter · ↑↓ pick · Enter select · Esc close";
            if (!_confirm.empty()) {
                hint = "y confirm · n cancel";
            }
            rows.push_back(hint_bar(hint));
            // Skills-modal pattern: the frame follows focus, so rows and
            // the form scroll when they exceed the modal height.
            return vbox(std::move(rows)) | vscroll_indicator | frame | xflex;
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

        std::string _selected;
        bool _picker_open = true;
        int _picker_begin = 0; // first visible row of the windowed list
        std::string _picker_buf;
        int _picker_cursor = 0;
        std::vector<std::string> _picker_ids;
        std::vector<std::string> _picker_labels;
        int _picker_selected = 0;
        Component _picker_input;

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
    };

} // namespace

ftxui::Component make_mcp(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<McpView>(std::move(state));
}

} // namespace imza
