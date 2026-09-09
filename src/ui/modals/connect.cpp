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
#include <cstdio>
#include <functional>
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
            : state_(std::move(state))
            , session_(state_->session)
            , provider_store_(providers)
        {
        }

        Element OnRender() override
        {
            sync_phase();
            if (entry_ == ConnectModal::Entry::PICK_MODEL) {
                maybe_rebuild_pick();
                return render_pick();
            }
            maybe_rebuild_manage();
            return render_manage();
        }

        bool OnEvent(Event event) override
        {
            if (entry_ == ConnectModal::Entry::PICK_MODEL) {
                return handle_pick_event(event);
            }
            return handle_manage_event(event);
        }

    private:
        struct PickSnap {
            ConnectionView view;
            ModelList list;
        };

        bool handle_pick_event(const Event& event)
        {
            if (event == Event::F5 || event == Event::CtrlR) {
                for (const auto& view : views()) {
                    provider_store_.refetch_models(view.id);
                }
                return true;
            }
            if (event == Event::ArrowDown) {
                pick_.move(1);
                return true;
            }
            if (event == Event::ArrowUp) {
                pick_.move(-1);
                return true;
            }
            if (event == Event::Return) {
                submit_pick();
                return true;
            }
            return container_ ? container_->OnEvent(event) : false;
        }

        bool handle_manage_event(const Event& event)
        {
            if (picker_open_) {
                if (event == Event::ArrowDown || event == Event::ArrowUp) {
                    move_list_cursor(event, picker_selected_,
                        static_cast<int>(picker_ids_.size()));
                    return true;
                }
                if (event == Event::Escape) {
                    close_picker();
                    return true;
                }
                if (event == Event::Return) {
                    commit_picker();
                    return true;
                }
                return container_ ? container_->OnEvent(event) : false;
            }

            bool confirming = false;
            for (const auto& entry : confirm_) {
                confirming = confirming || entry.second;
            }
            if (confirming) {
                if (event == Event::Character('y')) {
                    confirm_remove(row_selected_);
                    return true;
                }
                if (event == Event::Character('n') || event == Event::Escape) {
                    confirm_.clear();
                    rebuild_manage();
                    return true;
                }
                return container_ ? container_->OnEvent(event) : false;
            }

            const bool focus_in_add
                = add_container_ && add_container_->Focused();
            if (!in_add_ && !focus_in_add) {
                if (event == Event::ArrowDown) {
                    row_move(1);
                    return true;
                }
                if (event == Event::ArrowUp) {
                    row_move(-1);
                    return true;
                }
                if (event == Event::Character('d')) {
                    if (row_selected_ >= 0
                        && row_selected_ < static_cast<int>(views().size())) {
                        confirm_[row_selected_] = true;
                        rebuild_manage();
                    }
                    return true;
                }
            } else if (event == Event::ArrowUp && picker_input_
                && picker_input_->Focused()) {
                in_add_        = false;
                const auto all = views();
                row_selected_
                    = all.empty() ? 0 : static_cast<int>(all.size()) - 1;
                if (row_selected_ < static_cast<int>(row_buttons_.size())) {
                    row_buttons_[static_cast<std::size_t>(row_selected_)]
                        ->TakeFocus();
                }
                return true;
            }
            return container_ ? container_->OnEvent(event) : false;
        }

        void sync_phase()
        {
            const Session& st = *session_;
            const auto modal  = st.modal();
            if (const auto* m = std::get_if<ConnectModal>(&modal)) {
                if (m->entry != entry_) {
                    picker_open_  = false;
                    in_add_       = false;
                    row_selected_ = 0;
                    entry_        = m->entry;
                }
            }
        }

        std::vector<ConnectionView> views() const
        {
            return provider_store_.connections();
        }

        bool provider_connected(std::string_view provider_id)
        {
            for (const auto& view : views()) {
                if (view.provider == provider_id) {
                    return true;
                }
            }
            return false;
        }

        void row_move(int delta)
        {
            const auto all = views();
            if (all.empty()) {
                in_add_ = true;
                if (picker_input_) {
                    picker_input_->TakeFocus();
                }
                return;
            }
            if (delta > 0
                && row_selected_ >= static_cast<int>(all.size()) - 1) {
                in_add_ = true;
                if (picker_input_) {
                    picker_input_->TakeFocus();
                }
                return;
            }
            row_selected_ = std::clamp(
                row_selected_ + delta, 0, static_cast<int>(all.size()) - 1);
            if (row_selected_ < static_cast<int>(row_buttons_.size())) {
                row_buttons_[static_cast<std::size_t>(row_selected_)]
                    ->TakeFocus();
            }
        }

        std::string selected_provider_name()
        {
            for (const auto& [id, name] : providers_) {
                if (id == selected_provider_) {
                    return name;
                }
            }
            return "";
        }

        void refill_picker()
        {
            const std::string needle = to_lower(trim(picker_buf_));
            picker_labels_.clear();
            picker_ids_.clear();
            picker_selected_ = 0;
            for (const auto& [id, name] : providers_) {
                if (!needle.empty()
                    && to_lower(name).find(needle) == std::string::npos
                    && id.find(needle) == std::string::npos) {
                    continue;
                }
                picker_ids_.push_back(id);
                picker_labels_.push_back(name);
            }
        }

        void commit_picker()
        {
            if (picker_ids_.empty()
                || picker_selected_ >= static_cast<int>(picker_ids_.size())) {
                close_picker();
                return;
            }
            selected_provider_
                = picker_ids_[static_cast<std::size_t>(picker_selected_)];
            picker_buf_    = selected_provider_name();
            picker_cursor_ = static_cast<int>(picker_buf_.size());
            picker_open_   = false;
            rebuild_manage();
            if (subscription_signin_) {
                subscription_signin_->TakeFocus();
            } else if (key_input_) {
                key_input_->TakeFocus();
            }
        }

        void close_picker()
        {
            picker_buf_    = selected_provider_name();
            picker_cursor_ = static_cast<int>(picker_buf_.size());
            picker_open_   = false;
        }

        std::string current_endpoint()
        {
            if (selected_provider_ != CUSTOM_PROVIDER_ID) {
                return "";
            }
            return endpoint_for_base(strip_slash(trim(base_buf_)));
        }

        std::string current_signature()
        {
            return selected_provider_ + "|" + current_endpoint() + "|"
                + std::string(trim(key_buf_));
        }

        bool test_ok()
        {
            return tested_signature_ == current_signature()
                && session_->connect_status().rfind("✓", 0) == 0;
        }

        void maybe_rebuild_manage()
        {
            const Session& st = *session_;
            bool confirming   = false;
            for (const auto& entry : confirm_) {
                confirming = confirming || entry.second;
            }
            const bool base_visible = selected_provider_ == CUSTOM_PROVIDER_ID;
            const std::uint64_t status_key
                = std::hash<std::string> { }(st.connect_status()) << 32;
            const std::uint64_t key = status_key + st.modal_serial() * 16ULL
                + (confirming ? 4ULL : 0ULL) + (base_visible ? 2ULL : 0ULL)
                + (selected_provider_.empty() ? 0ULL : 1ULL);
            if (key == manage_key_) {
                return;
            }
            manage_key_ = key;
            rebuild_manage();
        }

        void rebuild_manage()
        {
            providers_ = provider_store_.provider_options();
            refill_picker();
            picker_input_ = Input(field_option(
                &picker_buf_, &picker_cursor_, "type to search providers",
                [this] {
                    row_error_.clear();
                    if (!picker_open_) {
                        picker_open_ = true;
                    }
                    refill_picker();
                },
                [this] {
                    if (picker_open_) {
                        commit_picker();
                    }
                }));

            const bool base_visible = selected_provider_ == CUSTOM_PROVIDER_ID;
            const bool subscription = subscription_provider(selected_provider_);
            label_input_ = Input(field_option(&label_buf_, &label_cursor_,
                provider_connected(selected_provider_)
                    ? "label (required — already connected)"
                    : "label (optional), e.g. my Ollama",
                [this] { row_error_.clear(); }));
            base_input_  = Input(field_option(&base_buf_, &base_cursor_,
                "base URL, e.g. http://localhost:1234/v1",
                [this] { row_error_.clear(); }));
            key_input_   = Input(password_option(&key_buf_, &key_cursor_,
                base_visible ? "API key (optional)" : "API key",
                [this] { row_error_.clear(); }));

            const auto on_action
                = [this] { test_ok() ? run_save() : run_test(); };
            action_button_
                = action_button(test_ok() ? "Save" : "Test", on_action);

            Components rows;
            const auto all = views();
            row_buttons_.clear();
            for (int i = 0; i < static_cast<int>(all.size()); ++i) {
                rows.push_back(make_row(i));
            }
            rows_container_ = Container::Vertical(std::move(rows));

            Components add_parts;
            add_parts.push_back(picker_input_);
            add_parts.push_back(label_input_);
            if (base_visible) {
                add_parts.push_back(base_input_);
            }
            if (subscription) {
                if (subscription_id_ != selected_provider_) {
                    subscription_signin_
                        = make_subscription_signin(state_, selected_provider_,
                            [this] { return std::string(trim(label_buf_)); });
                    subscription_id_ = selected_provider_;
                }
                add_parts.push_back(subscription_signin_);
            } else {
                subscription_signin_.reset();
                subscription_id_.clear();
                add_parts.push_back(key_input_);
                add_parts.push_back(action_button_);
            }
            add_container_ = Container::Vertical(std::move(add_parts));

            container_
                = Container::Vertical({ rows_container_, add_container_ });

            if (in_add_) {
                if (picker_input_) {
                    picker_input_->TakeFocus();
                }
            } else if (!row_buttons_.empty()) {
                row_buttons_[static_cast<std::size_t>(std::min(row_selected_,
                                 static_cast<int>(row_buttons_.size()) - 1))]
                    ->TakeFocus();
            }
        }

        Component make_row(int index)
        {
            const bool is_confirm
                = confirm_.count(index) != 0 && confirm_.at(index);

            Component label = Renderer([this, index] {
                const auto all = views();
                if (index >= 0 && index < static_cast<int>(all.size())) {
                    const ConnectionView& view
                        = all[static_cast<std::size_t>(index)];
                    const bool highlighted = index == row_selected_ && !in_add_;
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
                    "Yes", [this, index] { confirm_remove(index); });
                Component no = action_button("No", [this, index] {
                    confirm_.erase(index);
                    rebuild_manage();
                });
                right        = Container::Horizontal(
                    { yes, Renderer([] { return text(" "); }), no });
            } else {
                right = action_button("Remove", [this, index] {
                    confirm_[index] = true;
                    rebuild_manage();
                });
            }

            Component row = Container::Horizontal({ label, right });
            row->SetActiveChild(right);
            row_buttons_.push_back(right);
            return Renderer(row, [row, this, index, is_confirm] {
                Element e = row->Render() | xflex;
                if (is_confirm || (index == row_selected_ && !in_add_)) {
                    e |= bgcolor(PANEL_COLOR_FOCUS);
                }
                return e;
            });
        }

        void confirm_remove(int index)
        {
            const auto all = views();
            if (index < 0 || index >= static_cast<int>(all.size())
                || !provider_store_.remove_connection(
                    static_cast<std::size_t>(index),
                    all[static_cast<std::size_t>(index)].id)) {
                row_error_ = "Cannot remove the last connection.";
                confirm_.erase(index);
                rebuild_manage();
                return;
            }
            confirm_.erase(index);
            row_error_.clear();
            row_selected_ = 0;
            rebuild_manage();
        }

        void run_test()
        {
            const ConnectResult res = build_result(false);
            if (res.id.empty()) {
                return;
            }
            tested_signature_ = current_signature();
            imza::resolve_modal(*state_, ModalResult { res });
        }

        void run_save()
        {
            const ConnectResult res = build_result(true);
            if (res.id.empty()) {
                return;
            }
            imza::resolve_modal(*state_, ModalResult { res });
        }

        ConnectResult build_result(bool persist)
        {
            ConnectResult res;
            res.id      = selected_provider_;
            res.persist = persist;
            if (res.id.empty()) {
                row_error_ = "Select a provider.";
                return res;
            }
            res.endpoint = current_endpoint();
            if ((res.id == CUSTOM_PROVIDER_ID) && res.endpoint.empty()) {
                row_error_ = "Enter a base URL.";
                res.id     = "";
                return res;
            }
            res.api_key = trim(key_buf_);
            res.label   = trim(label_buf_);
            if (res.label.find('/') != std::string::npos) {
                row_error_ = "Label cannot contain '/'.";
                res.id     = "";
                return res;
            }
            const std::string key
                = res.label.empty() ? res.id : res.id + "/" + res.label;
            for (const auto& view : views()) {
                if (view.id == key) {
                    row_error_ = "Already connected — use a different label.";
                    res.id     = "";
                    return res;
                }
            }
            row_error_.clear();
            return res;
        }

        Element picker_area()
        {
            const std::string pad(FORM_LABEL + 2, ' ');
            Elements rows;
            rows.push_back(picker_input_->Render() | xflex);
            if (picker_open_) {
                rows.push_back(
                    text(pad + std::to_string(picker_ids_.size()) + " provider"
                        + (picker_ids_.size() == 1 ? "" : "s"))
                    | dim);
                if (picker_ids_.empty()) {
                    rows.push_back(text(pad + "no matching providers") | dim);
                } else {
                    for (int i = 0; i < static_cast<int>(picker_labels_.size());
                        ++i) {
                        const bool selected = i == picker_selected_;
                        Element e = text(pad + (selected ? "› " : "  ")
                            + picker_labels_[static_cast<std::size_t>(i)]);
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

        Element render_manage()
        {
            Elements rows = modal_header("Connections");

            const auto all = views();
            if (all.empty()) {
                rows.push_back(text("  (none — add one below)") | dim);
            } else {
                rows.push_back(hbox({
                    name_cell(views(), "Providers") | bold,
                    text(fit("Key", COL_KEY)) | dim,
                    text(fit("Status", COL_STATE)) | dim,
                    text(fit("Models", COL_MODELS)) | dim,
                    text(fit("Action", COL_ACTION)) | dim,
                }));
            }
            if (rows_container_ != nullptr) {
                rows.push_back(rows_container_->Render() | yflex);
            }

            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Add Provider"),
                text(subscription_provider(selected_provider_)
                        ? "  sign in using your existing subscription"
                        : "  pick a provider, paste your key, test, then save")
                    | dim,
            }));
            if (add_container_ != nullptr) {
                rows.push_back(hbox({
                    form_gutter("Provider"),
                    picker_area() | xflex,
                }));
                rows.push_back(hbox({
                    form_gutter("Label"),
                    label_input_->Render() | xflex,
                }));
                if (selected_provider_ == CUSTOM_PROVIDER_ID) {
                    rows.push_back(hbox({
                        form_gutter("Base URL"),
                        base_input_->Render() | xflex,
                    }));
                }
                if (subscription_provider(selected_provider_)) {
                    rows.push_back(hbox({
                        text(std::string(FORM_LABEL + 2, ' ')),
                        subscription_signin_->Render() | xflex,
                    }));
                } else {
                    rows.push_back(hbox({
                        form_gutter("API Key"),
                        key_input_->Render() | xflex,
                    }));
                    rows.push_back(hbox({
                        text(std::string(FORM_LABEL + 2, ' ')),
                        action_button_->Render(),
                        text("  "),
                        status_line_element(),
                    }));
                }
            }
            rows.push_back(separatorEmpty());
            std::string hint = "↑↓ navigate · Enter/d remove · Esc close";
            if (!confirm_.empty()) {
                hint = "y confirm · n cancel";
            }
            rows.push_back(hint_bar(hint));
            return vbox(std::move(rows)) | xflex;
        }

        Element status_line_element()
        {
            const Session& st = *session_;
            if (!row_error_.empty()) {
                return status_element(row_error_, false);
            }
            const bool fresh = tested_signature_ == current_signature();
            if (fresh && !st.connect_status().empty()) {
                const bool ok = st.connect_status().rfind("✓", 0) == 0;
                return status_element(st.connect_status(), ok);
            }
            return text("");
        }

        void maybe_rebuild_pick()
        {
            const Session& st = *session_;
            const Config cfg  = provider_store_.config();
            std::uint64_t key = st.modal_serial() * 1000003ULL;
            key += std::hash<std::string> { }(cfg.last_used
                    ? cfg.last_used->provider + " " + cfg.last_used->model
                    : std::string { });
            std::vector<PickSnap> snapshot;
            for (const auto& view : views()) {
                PickSnap snap;
                snap.view = view;
                snap.list = provider_store_.models_for(view.id);
                key += std::hash<std::string> { }(view.id)
                        * (static_cast<std::uint64_t>(
                               static_cast<int>(snap.list.state))
                            + 7ULL)
                    + snap.list.models.size() * 101ULL;
                snapshot.push_back(std::move(snap));
            }
            key += snapshot.size() * 7919ULL;
            if (key == pick_key_) {
                return;
            }
            pick_key_      = key;
            pick_snapshot_ = std::move(snapshot);
            rebuild_pick();
        }

        void rebuild_pick()
        {
            pick_.rows.clear();
            for (const auto& snap : pick_snapshot_) {
                for (const ModelInfo& info : snap.list.models) {
                    ModelRow row
                        = make_model_row(snap.view.id, snap.view.name, info);
                    if (info.context_length && *info.context_length > 0) {
                        row.tag += " · " + compact_number(*info.context_length);
                    }
                    pick_.rows.push_back(std::move(row));
                }
            }
            const Config cfg = provider_store_.config();
            if (cfg.last_used && !cfg.last_used->model.empty()) {
                for (std::size_t i = 0; i < pick_.rows.size(); ++i) {
                    ModelRow& row = pick_.rows[i];
                    if (row.connection_id == cfg.last_used->provider
                        && row.model_id == cfg.last_used->model) {
                        row.tag += " · current";
                        std::rotate(pick_.rows.begin(),
                            pick_.rows.begin() + static_cast<std::ptrdiff_t>(i),
                            pick_.rows.end());
                        break;
                    }
                }
            }
            pick_.selected = 0;
            pick_.refill_visible();

            pick_filter_ = Input(field_option(
                &pick_.filter, &pick_.filter_cursor, "filter models", [this] {
                    pick_.selected = 0;
                    pick_.refill_visible();
                }));

            container_ = Container::Vertical({ pick_filter_ });
        }

        void submit_pick()
        {
            const ModelRow* row = pick_.chosen();
            if (!row) {
                return;
            }
            imza::resolve_modal(*state_,
                ModalResult {
                    ModelChoice { row->connection_id, row->model_id } });
        }

        Element render_pick()
        {
            Elements rows = modal_header("Models");
            rows.push_back(pick_filter_->Render() | xflex);

            bool any_fetching = false;
            bool any_failed   = false;
            for (const auto& snap : pick_snapshot_) {
                if (snap.list.state == ModelList::State::FETCHING) {
                    any_fetching = true;
                }
                if (snap.list.state == ModelList::State::FAILED) {
                    any_failed = true;
                }
            }
            if (any_fetching) {
                rows.push_back(text("⟳ fetching providers…") | dim);
            }

            if (pick_.visible.empty()) {
                if (!any_fetching) {
                    rows.push_back(any_failed
                            ? status_element("✗ Some providers failed — press "
                                             "F5 to retry.",
                                  false)
                            : text("no models") | dim);
                }
            } else {
                for (int i = 0; i < static_cast<int>(pick_.visible.size());
                    ++i) {
                    const ModelRow& row
                        = pick_
                              .rows[pick_.visible[static_cast<std::size_t>(i)]];
                    rows.push_back(model_picker_row(row, i == pick_.selected));
                }
            }

            rows.push_back(separatorEmpty());
            rows.push_back(hint_bar("Enter pick · F5 refresh · Esc close"));
            return vbox(std::move(rows)) | xflex;
        }

        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        ProviderStore& provider_store_;
        ConnectModal::Entry entry_ = ConnectModal::Entry::MANAGE;

        Component container_;
        Component rows_container_;
        Component add_container_;
        std::vector<Component> row_buttons_;
        std::uint64_t manage_key_ = 0;
        std::uint64_t pick_key_   = 0;

        std::vector<std::pair<std::string, std::string>> providers_;
        std::string selected_provider_;
        bool picker_open_ = false;
        std::string picker_buf_;
        int picker_cursor_ = 0;
        std::vector<std::string> picker_labels_;
        std::vector<std::string> picker_ids_;
        int picker_selected_ = 0;
        Component picker_input_;

        bool in_add_      = false;
        int row_selected_ = 0;

        Component base_input_;
        Component label_input_;
        Component key_input_;
        Component action_button_;
        Component subscription_signin_;
        std::string subscription_id_;
        std::string base_buf_;
        int base_cursor_ = 0;
        std::string label_buf_;
        int label_cursor_ = 0;
        std::string key_buf_;
        int key_cursor_ = 0;
        std::string tested_signature_;
        std::map<int, bool> confirm_;
        std::string row_error_;

        std::vector<PickSnap> pick_snapshot_;
        ModelPickList pick_;
        Component pick_filter_;
    };

} // namespace

ftxui::Component make_connect(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<ConnectView>(state, *state->providers);
}

} // namespace imza
