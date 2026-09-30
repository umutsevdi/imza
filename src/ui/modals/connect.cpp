#include "app/flows.h"
#include "common/types.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "providers/catalog.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace imza {

namespace {

    using namespace ftxui;

    constexpr int COL_KEY      = 14;
    constexpr int COL_STATE    = 8;
    constexpr int COL_MODELS   = 12;
    constexpr int COL_ACTION   = 12;
    constexpr int FORM_LABEL   = 9;
    constexpr int COL_NAME_MIN = 22;
    constexpr int COL_NAME_GAP = 2;

    Element name_cell(
        const std::vector<ConnectionView>& views, const std::string& name)
    {
        int width = COL_NAME_MIN;
        for (const auto& view : views) {
            width = std::max(
                width, static_cast<int>(utf8_width(view.name)) + COL_NAME_GAP);
        }
        return text(fit(name, width));
    }

    std::string mask_key(const std::string& key)
    {
        if (key.empty()) {
            return "(no key)";
        }
        if (key.size() <= 4) {
            return "••••";
        }
        return "••••••" + key.substr(key.size() - 4);
    }

    bool subscription_provider(std::string_view id)
    {
        return id == OPENAI_SUBSCRIPTION_ID;
    }

    Element status_element(const std::string& text_value, bool ok)
    {
        if (ok) {
            return text(text_value) | color(HL_GREEN);
        }
        return text(text_value) | color(HL_RED);
    }

    Element form_gutter(const std::string& label)
    {
        return hbox({ text("  "), text(fit(label, FORM_LABEL)) | bold });
    }

    class ConnectView : public ComponentBase {
    public:
        explicit ConnectView(
            std::shared_ptr<ApplicationState> state, ProviderStore& providers)
            : _state(std::move(state))
            , _session(_state->session)
            , _provider_store(providers)
        {
        }

        Element OnRender() override
        {
            _sync_phase();
            if (_entry == ConnectModal::Entry::PICK_MODEL) {
                _maybe_rebuild_pick();
                return _render_pick();
            }
            _maybe_rebuild_manage();
            return _render_manage();
        }

        bool OnEvent(Event event) override
        {
            if (_entry == ConnectModal::Entry::PICK_MODEL) {
                return _handle_pick_event(event);
            }
            return _handle_manage_event(event);
        }

    private:
        struct PickSnap {
            ConnectionView view;
            ModelList list;
        };

        bool _handle_pick_event(const Event& event)
        {
            if (event == Event::F5 || event == Event::CtrlR) {
                for (const auto& view : _views()) {
                    _provider_store.refetch_models(view.id);
                }
                return true;
            }
            return handle_model_pick_event(
                _pick, _container, event, [this] { _submit_pick(); });
        }

        bool _handle_manage_event(const Event& event)
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

            bool confirming = false;
            for (const auto& entry : _confirm) {
                confirming = confirming || entry.second;
            }
            if (confirming) {
                if (event == Event::Character('y')) {
                    _confirm_remove(_row_selected);
                    return true;
                }
                if (event == Event::Character('n') || event == Event::Escape) {
                    _confirm.clear();
                    _rebuild_manage();
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
                        && _row_selected < static_cast<int>(_views().size())) {
                        _confirm[_row_selected] = true;
                        _rebuild_manage();
                    }
                    return true;
                }
            } else if (event == Event::ArrowUp && _picker_input
                && _picker_input->Focused()) {
                _in_add        = false;
                const auto all = _views();
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

        void _sync_phase()
        {
            const Session& st = *_session;
            const auto modal  = st.modal();
            if (const auto* m = std::get_if<ConnectModal>(&modal)) {
                if (m->entry != _entry) {
                    _picker_open  = false;
                    _in_add       = false;
                    _row_selected = 0;
                    _entry        = m->entry;
                }
            }
        }

        std::vector<ConnectionView> _views() const
        {
            return _provider_store.connections();
        }

        bool _provider_connected(std::string_view provider_id)
        {
            for (const auto& view : _views()) {
                if (view.provider == provider_id) {
                    return true;
                }
            }
            return false;
        }

        void _row_move(int delta)
        {
            const auto all = _views();
            if (all.empty()) {
                _in_add = true;
                if (_picker_input) {
                    _picker_input->TakeFocus();
                }
                return;
            }
            if (delta > 0
                && _row_selected >= static_cast<int>(all.size()) - 1) {
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

        std::string _selected_provider_name()
        {
            for (const auto& [id, name] : _providers) {
                if (id == _selected_provider) {
                    return name;
                }
            }
            return "";
        }

        void _refill_picker()
        {
            const std::string needle = to_lower(trim(_picker_buf));
            _picker_labels.clear();
            _picker_ids.clear();
            _picker_selected = 0;
            for (const auto& [id, name] : _providers) {
                if (!needle.empty()
                    && to_lower(name).find(needle) == std::string::npos
                    && id.find(needle) == std::string::npos) {
                    continue;
                }
                _picker_ids.push_back(id);
                _picker_labels.push_back(name);
            }
        }

        void _commit_picker()
        {
            if (_picker_ids.empty()
                || _picker_selected >= static_cast<int>(_picker_ids.size())) {
                _close_picker();
                return;
            }
            _selected_provider
                = _picker_ids[static_cast<std::size_t>(_picker_selected)];
            _picker_buf    = _selected_provider_name();
            _picker_cursor = static_cast<int>(_picker_buf.size());
            _picker_open   = false;
            _rebuild_manage();
            if (_subscription_signin) {
                _subscription_signin->TakeFocus();
            } else if (_key_input) {
                _key_input->TakeFocus();
            }
        }

        void _close_picker()
        {
            _picker_buf    = _selected_provider_name();
            _picker_cursor = static_cast<int>(_picker_buf.size());
            _picker_open   = false;
        }

        std::string _current_endpoint()
        {
            if (_selected_provider != CUSTOM_PROVIDER_ID) {
                return "";
            }
            return endpoint_for_base(strip_slash(trim(_base_buf)));
        }

        std::string _current_signature()
        {
            return _selected_provider + "|" + _current_endpoint() + "|"
                + std::string(trim(_key_buf));
        }

        bool _test_ok()
        {
            return _tested_signature == _current_signature()
                && _session->connect_status().rfind("✓", 0) == 0;
        }

        void _maybe_rebuild_manage()
        {
            const Session& st = *_session;
            bool confirming   = false;
            for (const auto& entry : _confirm) {
                confirming = confirming || entry.second;
            }
            const bool base_visible = _selected_provider == CUSTOM_PROVIDER_ID;
            const std::uint64_t status_key
                = std::hash<std::string> { }(st.connect_status()) << 32;
            const std::uint64_t key = status_key + st.modal_serial() * 16ULL
                + (confirming ? 4ULL : 0ULL) + (base_visible ? 2ULL : 0ULL)
                + (_selected_provider.empty() ? 0ULL : 1ULL);
            if (key == _manage_key) {
                return;
            }
            _manage_key = key;
            _rebuild_manage();
        }

        void _rebuild_manage()
        {
            _providers = _provider_store.provider_options();
            _refill_picker();
            _picker_input = Input(field_option(
                &_picker_buf, &_picker_cursor, "type to search providers",
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

            const bool base_visible = _selected_provider == CUSTOM_PROVIDER_ID;
            const bool subscription = subscription_provider(_selected_provider);
            _label_input = Input(field_option(&_label_buf, &_label_cursor,
                _provider_connected(_selected_provider)
                    ? "label (required - already connected)"
                    : "label (optional), e.g. my Ollama",
                [this] { _row_error.clear(); }));
            _base_input  = Input(field_option(&_base_buf, &_base_cursor,
                "base URL, e.g. http://localhost:1234/v1",
                [this] { _row_error.clear(); }));
            _key_input   = Input(password_option(&_key_buf, &_key_cursor,
                base_visible ? "API key (optional)" : "API key",
                [this] { _row_error.clear(); }));

            const auto on_action = [this] { _run_action(_test_ok()); };
            _action_button
                = action_button(_test_ok() ? "Save" : "Test", on_action);

            Components rows;
            const auto all = _views();
            _row_buttons.clear();
            for (int i = 0; i < static_cast<int>(all.size()); ++i) {
                rows.push_back(_make_row(i));
            }
            _rows_container = Container::Vertical(std::move(rows));

            Components add_parts;
            add_parts.push_back(_picker_input);
            add_parts.push_back(_label_input);
            if (base_visible) {
                add_parts.push_back(_base_input);
            }
            if (subscription) {
                if (_subscription_id != _selected_provider) {
                    _subscription_signin
                        = make_subscription_signin(_state, _selected_provider,
                            [this] { return std::string(trim(_label_buf)); });
                    _subscription_id = _selected_provider;
                }
                add_parts.push_back(_subscription_signin);
            } else {
                _subscription_signin.reset();
                _subscription_id.clear();
                add_parts.push_back(_key_input);
                add_parts.push_back(_action_button);
            }
            _add_container = Container::Vertical(std::move(add_parts));

            _container
                = Container::Vertical({ _rows_container, _add_container });

            if (_in_add) {
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
                const auto all = _views();
                if (index >= 0 && index < static_cast<int>(all.size())) {
                    const ConnectionView& view
                        = all[static_cast<std::size_t>(index)];
                    const bool highlighted = index == _row_selected && !_in_add;
                    Element state_el       = text("");
                    if (view.state == ConnectionView::State::READY) {
                        state_el = text("✓") | color(HL_GREEN);
                    } else if (view.state == ConnectionView::State::FAILED) {
                        state_el = text("✗") | color(HL_RED);
                    } else {
                        state_el = text("⟳") | color(PANEL_FG_DIM);
                    }
                    std::string models;
                    if (view.state == ConnectionView::State::READY) {
                        models = std::to_string(view.model_count) + " models";
                    } else if (view.state == ConnectionView::State::FAILED) {
                        models = error_text(view.error);
                    } else {
                        models = "fetching…";
                    }
                    Element models_el = text(fit(models, COL_MODELS));
                    if (view.state == ConnectionView::State::FAILED) {
                        models_el
                            = status_element(fit(models, COL_MODELS), false);
                    } else {
                        models_el = std::move(models_el) | dim;
                    }
                    Element name_el = name_cell(all, view.name);
                    if (highlighted) {
                        name_el = std::move(name_el) | bold;
                    }
                    return hbox({
                        std::move(name_el),
                        text(fit(subscription_provider(view.provider)
                                ? "signed in"
                                : mask_key(view.api_key),
                            COL_KEY))
                            | dim,
                        state_el | size(WIDTH, EQUAL, COL_STATE),
                        std::move(models_el),
                    });
                }
                return text("");
            });

            Component right;
            if (is_confirm) {
                Component yes = action_button(
                    "Yes", [this, index] { _confirm_remove(index); });
                Component no = action_button("No", [this, index] {
                    _confirm.erase(index);
                    _rebuild_manage();
                });
                right        = Container::Horizontal(
                    { yes, Renderer([] { return text(" "); }), no });
            } else {
                right = action_button("Remove", [this, index] {
                    _confirm[index] = true;
                    _rebuild_manage();
                });
            }

            Component row = Container::Horizontal({ label, right });
            row->SetActiveChild(right);
            _row_buttons.push_back(right);
            return Renderer(row, [row, this, index, is_confirm] {
                Element e = row->Render() | xflex;
                if (is_confirm || (index == _row_selected && !_in_add)) {
                    e |= bgcolor(PANEL_COLOR_FOCUS);
                }
                return e;
            });
        }

        void _confirm_remove(int index)
        {
            const auto all = _views();
            if (index < 0 || index >= static_cast<int>(all.size())
                || !_provider_store.remove_connection(
                    static_cast<std::size_t>(index),
                    all[static_cast<std::size_t>(index)].id)) {
                _row_error = "Cannot remove the last connection.";
                _confirm.erase(index);
                _rebuild_manage();
                return;
            }
            _confirm.erase(index);
            _row_error.clear();
            _row_selected = 0;
            _rebuild_manage();
        }

        void _run_action(bool persist)
        {
            const ConnectResult res = _build_result(persist);
            if (res.id.empty()) {
                return;
            }
            // Only Test records the signature; Save already requires one.
            if (!persist) {
                _tested_signature = _current_signature();
            }
            imza::resolve_modal(*_state, ModalResult { res });
        }

        ConnectResult _build_result(bool persist)
        {
            ConnectResult res;
            res.id      = _selected_provider;
            res.persist = persist;
            if (res.id.empty()) {
                _row_error = "Select a provider.";
                return res;
            }
            res.endpoint = _current_endpoint();
            if ((res.id == CUSTOM_PROVIDER_ID) && res.endpoint.empty()) {
                _row_error = "Enter a base URL.";
                res.id     = "";
                return res;
            }
            res.api_key = trim(_key_buf);
            res.label   = trim(_label_buf);
            if (res.label.find('/') != std::string::npos) {
                _row_error = "Label cannot contain '/'.";
                res.id     = "";
                return res;
            }
            const std::string key = connection_key_for(res.id, res.label);
            for (const auto& view : _views()) {
                if (view.id == key) {
                    _row_error = "Already connected - use a different label.";
                    res.id     = "";
                    return res;
                }
            }
            _row_error.clear();
            return res;
        }

        Element _picker_area()
        {
            const std::string pad(FORM_LABEL + 2, ' ');
            Elements rows;
            rows.push_back(_picker_input->Render() | xflex);
            if (_picker_open) {
                rows.push_back(
                    text(pad + std::to_string(_picker_ids.size()) + " provider"
                        + (_picker_ids.size() == 1 ? "" : "s"))
                    | dim);
                if (_picker_ids.empty()) {
                    rows.push_back(text(pad + "no matching providers") | dim);
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

        Element _render_manage()
        {
            Elements rows = modal_header("Connections");

            const auto all = _views();
            if (all.empty()) {
                rows.push_back(text("  (none - add one below)") | dim);
            } else {
                rows.push_back(hbox({
                    name_cell(_views(), "Providers") | bold,
                    text(fit("Key", COL_KEY)) | dim,
                    text(fit("Status", COL_STATE)) | dim,
                    text(fit("Models", COL_MODELS)) | dim,
                    text(fit("Action", COL_ACTION)) | dim,
                }));
            }
            if (_rows_container != nullptr) {
                rows.push_back(_rows_container->Render() | yflex);
            }

            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Add Provider"),
                text(subscription_provider(_selected_provider)
                        ? "  sign in using your existing subscription"
                        : "  pick a provider, paste your key, test, then save")
                    | dim,
            }));
            if (_add_container != nullptr) {
                rows.push_back(hbox({
                    form_gutter("Provider"),
                    _picker_area() | xflex,
                }));
                rows.push_back(hbox({
                    form_gutter("Label"),
                    _label_input->Render() | xflex,
                }));
                if (_selected_provider == CUSTOM_PROVIDER_ID) {
                    rows.push_back(hbox({
                        form_gutter("Base URL"),
                        _base_input->Render() | xflex,
                    }));
                }
                if (subscription_provider(_selected_provider)) {
                    rows.push_back(hbox({
                        text(std::string(FORM_LABEL + 2, ' ')),
                        _subscription_signin->Render() | xflex,
                    }));
                } else {
                    rows.push_back(hbox({
                        form_gutter("API Key"),
                        _key_input->Render() | xflex,
                    }));
                    rows.push_back(hbox({
                        text(std::string(FORM_LABEL + 2, ' ')),
                        _action_button->Render(),
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

        Element _status_line_element()
        {
            const Session& st = *_session;
            if (!_row_error.empty()) {
                return status_element(_row_error, false);
            }
            const bool fresh = _tested_signature == _current_signature();
            if (fresh && !st.connect_status().empty()) {
                const bool ok = st.connect_status().rfind("✓", 0) == 0;
                return status_element(st.connect_status(), ok);
            }
            return text("");
        }

        void _maybe_rebuild_pick()
        {
            const Session& st = *_session;
            const Config cfg  = _provider_store.config();
            std::uint64_t key = st.modal_serial() * 1000003ULL;
            key += std::hash<std::string> { }(cfg.last_used
                    ? cfg.last_used->provider + " " + cfg.last_used->model
                    : std::string { });
            std::vector<PickSnap> snapshot;
            for (const auto& view : _views()) {
                PickSnap snap;
                snap.view = view;
                snap.list = _provider_store.models_for(view.id);
                key += std::hash<std::string> { }(view.id)
                        * (static_cast<std::uint64_t>(
                               static_cast<int>(snap.list.state))
                            + 7ULL)
                    + snap.list.models.size() * 101ULL;
                snapshot.push_back(std::move(snap));
            }
            key += snapshot.size() * 7919ULL;
            if (key == _pick_key) {
                return;
            }
            _pick_key      = key;
            _pick_snapshot = std::move(snapshot);
            _rebuild_pick();
        }

        void _rebuild_pick()
        {
            _pick.rows.clear();
            for (const auto& snap : _pick_snapshot) {
                for (const ModelInfo& info : snap.list.models) {
                    ModelRow row
                        = make_model_row(snap.view.id, snap.view.name, info);
                    if (info.context_length && *info.context_length > 0) {
                        row.tag += " · " + compact_number(*info.context_length);
                    }
                    _pick.rows.push_back(std::move(row));
                }
            }
            const Config cfg = _provider_store.config();
            if (cfg.last_used && !cfg.last_used->model.empty()) {
                for (std::size_t i = 0; i < _pick.rows.size(); ++i) {
                    ModelRow& row = _pick.rows[i];
                    if (row.connection_id == cfg.last_used->provider
                        && row.model_id == cfg.last_used->model) {
                        row.tag += " · current";
                        std::rotate(_pick.rows.begin(),
                            _pick.rows.begin() + static_cast<std::ptrdiff_t>(i),
                            _pick.rows.end());
                        break;
                    }
                }
            }
            _pick.selected = 0;
            _pick.refill_visible();

            _pick_filter = make_model_pick_filter(_pick);

            _container = Container::Vertical({ _pick_filter });
        }

        void _submit_pick()
        {
            const ModelRow* row = _pick.chosen();
            if (!row) {
                return;
            }
            imza::resolve_modal(*_state,
                ModalResult {
                    ModelChoice { row->connection_id, row->model_id } });
        }

        Element _render_pick()
        {
            bool any_fetching = false;
            bool any_failed   = false;
            for (const auto& snap : _pick_snapshot) {
                if (snap.list.state == ModelList::State::FETCHING) {
                    any_fetching = true;
                }
                if (snap.list.state == ModelList::State::FAILED) {
                    any_failed = true;
                }
            }
            Element empty_state;
            if (!any_fetching) {
                empty_state = any_failed
                    ? status_element("✗ Some providers failed - press "
                                     "F5 to retry.",
                          false)
                    : text("no models") | dim;
            }
            Element status_row = any_fetching
                ? text("⟳ fetching providers…") | dim
                : Element { };
            return render_model_pick("Models", _pick_filter, _pick,
                std::move(status_row), std::move(empty_state),
                "Enter pick · F5 refresh · Esc close");
        }

        std::shared_ptr<ApplicationState> _state;
        std::shared_ptr<Session> _session;
        ProviderStore& _provider_store;
        ConnectModal::Entry _entry = ConnectModal::Entry::MANAGE;

        Component _container;
        Component _rows_container;
        Component _add_container;
        std::vector<Component> _row_buttons;
        std::uint64_t _manage_key = 0;
        std::uint64_t _pick_key   = 0;

        std::vector<std::pair<std::string, std::string>> _providers;
        std::string _selected_provider;
        bool _picker_open = false;
        std::string _picker_buf;
        int _picker_cursor = 0;
        std::vector<std::string> _picker_labels;
        std::vector<std::string> _picker_ids;
        int _picker_selected = 0;
        Component _picker_input;

        bool _in_add      = false;
        int _row_selected = 0;

        Component _base_input;
        Component _label_input;
        Component _key_input;
        Component _action_button;
        Component _subscription_signin;
        std::string _subscription_id;
        std::string _base_buf;
        int _base_cursor = 0;
        std::string _label_buf;
        int _label_cursor = 0;
        std::string _key_buf;
        int _key_cursor = 0;
        std::string _tested_signature;
        std::map<int, bool> _confirm;
        std::string _row_error;

        std::vector<PickSnap> _pick_snapshot;
        ModelPickList _pick;
        Component _pick_filter;
    };

} // namespace

ftxui::Component make_connect(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<ConnectView>(state, *state->providers);
}

} // namespace imza
