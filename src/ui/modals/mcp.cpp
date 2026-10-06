#include "app/flows.h"
#include "common/modal.h"
#include "common/types.h"
#include "common/util.h"
#include "network/web.h"
#include "platform/config.h"
#include "tools/mcp_manager.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace imza {

namespace {

    using namespace ftxui;

    constexpr int COL_STATE = 10;
    constexpr int COL_TOOLS = 8;

    Element status_element(const std::string& text_value, bool ok)
    {
        if (ok) {
            return text(text_value) | color(HL_GREEN);
        }
        return text(text_value) | color(HL_RED);
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

    class McpView : public ComponentBase {
    public:
        explicit McpView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
            , _session(_state->session)
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
            return _container ? _container->OnEvent(event) : false;
        }

    private:
        std::string _current_serial()
        {
            // Cheap refresh key: the modal serial plus the snapshot row
            // count. State changes bump the serial via the subscription.
            McpManager* mcp = _state->mcp.get();
            return std::to_string(_session->modal_serial()) + "/"
                + std::to_string(mcp == nullptr ? 0 : mcp->snapshot().size());
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

        void _rebuild()
        {
            Components rows;
            _row_buttons.clear();
            if (_state->mcp != nullptr) {
                for (const auto& server : _state->mcp->snapshot()) {
                    rows.push_back(_make_row(server));
                }
            }
            _rows_container = Container::Vertical(std::move(rows));

            _id_input    = Input(field_option(&_id_buf, &_id_cursor,
                "id, e.g. exa (used by imza.mcp.call)",
                [this] { _row_error.clear(); }));
            _label_input = Input(field_option(&_label_buf, &_label_cursor,
                "label (optional)", [this] { _row_error.clear(); }));
            _url_input   = Input(field_option(&_url_buf, &_url_cursor,
                "URL, e.g. https://mcp.exa.ai/mcp",
                [this] { _row_error.clear(); }));
            _token_input = Input(password_option(&_token_buf, &_token_cursor,
                "bearer token (optional)", [this] { _row_error.clear(); }));
            _add_button  = action_button("Add", [this] { _add_server(); });

            _add_container = Container::Vertical({ _id_input, _label_input,
                _url_input, _token_input, _add_button });
            _container
                = Container::Vertical({ _rows_container, _add_container });
        }

        Component _make_row(const McpServerSnapshot& server)
        {
            const int index = static_cast<int>(_row_buttons.size());
            const bool confirming
                = _confirm.count(index) != 0 && _confirm.at(index);

            Component label = Renderer([server] {
                Element name = text(server.id) | bold;
                if (server.label != server.id && !server.label.empty()) {
                    name = text(server.id + " (" + server.label + ")") | bold;
                }
                Element detail;
                if (server.state == McpServerState::FAILED) {
                    detail = status_element(fit(state_text(server), 40), false);
                } else {
                    std::string value = state_text(server);
                    detail            = text(fit(value, COL_TOOLS)) | dim;
                }
                return hbox({ std::move(name),
                    text("  ") | size(WIDTH, EQUAL, 1),
                    state_glyph(server.state) | size(WIDTH, EQUAL, COL_STATE),
                    std::move(detail) });
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
                McpManager* mcp = _state->mcp.get();
                if (mcp != nullptr) {
                    if (server.state == McpServerState::CONNECTED
                        || server.state == McpServerState::FAILED) {
                        buttons.push_back(
                            action_button("Disconnect", [this, id = server.id] {
                                if (_state->mcp) {
                                    _state->mcp->disconnect(id);
                                }
                            }));
                    } else if (server.state != McpServerState::CONNECTING) {
                        buttons.push_back(
                            action_button("Connect", [this, id = server.id] {
                                if (_state->mcp) {
                                    _state->mcp->connect(id);
                                }
                            }));
                    }
                }
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

        void _confirm_remove(int index)
        {
            _confirm.erase(index);
            auto snapshot = _state->mcp ? _state->mcp->snapshot()
                                        : std::vector<McpServerSnapshot> { };
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
                _row_error = "Could not save the config.";
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
            std::string id = sanitize_id(std::string(trim(_id_buf)));
            if (id.empty()) {
                _row_error = "Enter a server id.";
                return;
            }
            for (const auto& server : _state->mcp
                    ? _state->mcp->snapshot()
                    : std::vector<McpServerSnapshot> { }) {
                if (server.id == id) {
                    _row_error = "Id already in use.";
                    return;
                }
            }
            std::string url;
            if (normalize_web_url(std::string(trim(_url_buf)), url)
                != Status::OK) {
                _row_error = "Enter an http(s) URL.";
                return;
            }

            McpServerConfig server;
            server.id           = id;
            server.label        = trim(_label_buf);
            server.url          = url;
            server.bearer_token = trim(_token_buf);
            server.enabled      = true;

            Config initial = _state->providers->config();
            Config result;
            const ConfigUpdateResult updated = update_config(
                config_path(), initial,
                [&server](Config& cfg) {
                    return cfg.mcp_servers.insert_or_assign(server.id, server)
                        .second;
                },
                &result);
            if (updated != ConfigUpdateResult::UPDATED) {
                _row_error = "Could not save the config.";
                return;
            }

            _row_error.clear();
            _id_buf.clear();
            _label_buf.clear();
            _url_buf.clear();
            _token_buf.clear();
            if (_state->mcp) {
                _state->mcp->reload(std::move(result.mcp_servers));
                _state->mcp->connect(id);
            }
        }

        Element _render()
        {
            Elements rows = modal_header("MCP Servers");
            rows.push_back(
                text("  Servers from config.json; call their tools via the lua "
                     "mcp module.")
                | dim);
            if (_rows_container != nullptr) {
                rows.push_back(_rows_container->Render() | yflex);
            }

            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Add Server"),
                text("  endpoint of a Streamable-HTTP MCP server") | dim,
            }));
            if (_add_container != nullptr) {
                rows.push_back(hbox({
                    text("  "),
                    vbox({ _id_input->Render() | xflex,
                        _label_input->Render() | xflex,
                        _url_input->Render() | xflex,
                        _token_input->Render() | xflex })
                        | xflex,
                }));
                rows.push_back(hbox({
                    text("  "),
                    _add_button->Render(),
                    text("  "),
                    _row_error.empty() ? text("")
                                       : status_element(_row_error, false),
                }));
            }
            rows.push_back(separatorEmpty());
            std::string hint = "Esc close · Enter press focused button";
            if (!_confirm.empty()) {
                hint = "y confirm · n cancel";
            }
            rows.push_back(hint_bar(hint));
            return vbox(std::move(rows)) | xflex;
        }

        std::shared_ptr<ApplicationState> _state;
        std::shared_ptr<Session> _session;
        Signal<>::Subscription _subscription;

        Component _container;
        Component _rows_container;
        Component _add_container;
        std::vector<Component> _row_buttons;
        std::map<int, bool> _confirm;
        std::string _row_error;
        std::string _serial;
        bool _built = false;

        Component _id_input;
        Component _label_input;
        Component _url_input;
        Component _token_input;
        Component _add_button;
        std::string _id_buf;
        int _id_cursor = 0;
        std::string _label_buf;
        int _label_cursor = 0;
        std::string _url_buf;
        int _url_cursor = 0;
        std::string _token_buf;
        int _token_cursor = 0;
    };

} // namespace

ftxui::Component make_mcp(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<McpView>(std::move(state));
}

} // namespace imza
