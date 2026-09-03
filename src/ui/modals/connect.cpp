#include "agent/flows.h"
#include "subsystems/session_store.h"
#include "core/catalog.h"
#include "common/types.h"
#include "ui/ui.h"
#include "common/util.h"

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

namespace ursa {

namespace {

    using namespace ftxui;

    constexpr int kColName   = 22;
    constexpr int kColKey    = 14;
    constexpr int kColState  = 8;
    constexpr int kColModels = 12;
    constexpr int kColAction = 12;
    constexpr int kFormLabel = 9;

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

    Element status_element(const std::string& text_value, bool ok)
    {
        if (ok) {
            return text(text_value) | color(Color::GreenLight);
        }
        return text(text_value) | color(Color::RedLight);
    }

    Element form_gutter(const std::string& label)
    {
        return hbox({ text("  "), text(fit(label, kFormLabel)) | bold });
    }

    class ConnectView : public ComponentBase {
    public:
        explicit ConnectView(std::shared_ptr<ApplicationState> state,
            ProviderStore& providers)
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
                if (event == Event::ArrowDown) {
                    if (!picker_ids_.empty()) {
                        picker_selected_ = std::min(picker_selected_ + 1,
                            static_cast<int>(picker_ids_.size()) - 1);
                    }
                    return true;
                }
                if (event == Event::ArrowUp) {
                    picker_selected_ = std::max(picker_selected_ - 1, 0);
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
            for (const auto& [id, active] : confirm_) {
                confirming = confirming || active;
            }
            if (confirming) {
                if (event == Event::Character('y')) {
                    confirm_remove(row_id_at(row_selected_));
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
                    const std::string id = row_id_at(row_selected_);
                    if (!id.empty()) {
                        confirm_[id] = true;
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

        std::string row_id_at(int index)
        {
            const auto all = views();
            if (index < 0 || index >= static_cast<int>(all.size())) {
                return "";
            }
            return all[static_cast<std::size_t>(index)].id;
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
            if (key_input_) {
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
            if (selected_provider_ != kCustomProviderId
                && selected_provider_ != kLocalProviderId) {
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
            for (const auto& [id, active] : confirm_) {
                confirming = confirming || active;
            }
            const bool base_visible = selected_provider_ == kCustomProviderId
                || selected_provider_ == kLocalProviderId;
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
            if (selected_provider_ == kLocalProviderId
                && trim(base_buf_).empty()) {
                base_buf_ = "http://localhost:11434/v1";
            }

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

            const bool base_visible = selected_provider_ == kCustomProviderId
                || selected_provider_ == kLocalProviderId;
            base_input_ = Input(field_option(&base_buf_, &base_cursor_,
                "base URL, e.g. http://localhost:1234/v1",
                [this] { row_error_.clear(); }));
            key_input_  = Input(password_option(&key_buf_, &key_cursor_,
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
                rows.push_back(
                    make_row(all[static_cast<std::size_t>(i)].id, i));
            }
            rows_container_ = Container::Vertical(std::move(rows));

            Components add_parts;
            add_parts.push_back(picker_input_);
            if (base_visible) {
                add_parts.push_back(base_input_);
            }
            add_parts.push_back(key_input_);
            add_parts.push_back(action_button_);
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

        Component make_row(const std::string& id, int index)
        {
            bool is_confirm = confirm_.count(id) != 0 && confirm_.at(id);

            Component label = Renderer([this, id, index] {
                for (const auto& view : views()) {
                    if (view.id != id) {
                        continue;
                    }
                    const bool highlighted = index == row_selected_ && !in_add_;
                    Element state_el       = text("");
                    if (view.state == ConnectionView::State::READY) {
                        state_el = text("✓") | color(Color::GreenLight);
                    } else if (view.state == ConnectionView::State::FAILED) {
                        state_el = text("✗") | color(Color::RedLight);
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
                    Element models_el = text(fit(models, kColModels));
                    if (view.state == ConnectionView::State::FAILED) {
                        models_el
                            = status_element(fit(models, kColModels), false);
                    } else {
                        models_el = std::move(models_el) | dim;
                    }
                    Element name_el = text(fit(view.name, kColName));
                    if (highlighted) {
                        name_el = std::move(name_el) | bold;
                    }
                    return hbox({
                        std::move(name_el),
                        text(fit(mask_key(view.api_key), kColKey)) | dim,
                        state_el | size(WIDTH, EQUAL, kColState),
                        std::move(models_el),
                    });
                }
                return text("");
            });

            Component right;
            if (is_confirm) {
                Component yes
                    = action_button("Yes", [this, id] { confirm_remove(id); });
                Component no = action_button("No", [this, id] {
                    confirm_.erase(id);
                    rebuild_manage();
                });
                right        = Container::Horizontal(
                    { yes, Renderer([] { return text(" "); }), no });
            } else {
                right = action_button("Remove", [this, id] {
                    confirm_[id] = true;
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

        void confirm_remove(const std::string& id)
        {
            if (!provider_store_.remove_connection(id)) {
                row_error_ = "Cannot remove the last connection.";
                confirm_.erase(id);
                rebuild_manage();
                return;
            }
            confirm_.erase(id);
            row_error_.clear();
            row_selected_ = 0;
            rebuild_manage();
        }

        void run_test()
        {
            const ConnectResult res = build_result(false);
            if (res.provider_id.empty()) {
                return;
            }
            tested_signature_ = current_signature();
            ursa::resolve_modal(*state_, ModalResult { res });
        }

        void run_save()
        {
            const ConnectResult res = build_result(true);
            if (res.provider_id.empty()) {
                return;
            }
            ursa::resolve_modal(*state_, ModalResult { res });
        }

        ConnectResult build_result(bool persist)
        {
            ConnectResult res;
            res.provider_id = selected_provider_;
            res.persist     = persist;
            if (res.provider_id.empty()) {
                row_error_ = "Select a provider.";
                return res;
            }
            res.endpoint = current_endpoint();
            if ((res.provider_id == kCustomProviderId
                    || res.provider_id == kLocalProviderId)
                && res.endpoint.empty()) {
                row_error_      = "Enter a base URL.";
                res.provider_id = "";
                return res;
            }
            res.api_key = trim(key_buf_);
            row_error_.clear();
            return res;
        }

        Element picker_area()
        {
            const std::string pad(kFormLabel + 2, ' ');
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
            Elements rows { text("Connections") | bold, separatorEmpty() };

            const auto all = views();
            if (all.empty()) {
                rows.push_back(text("  (none — add one below)") | dim);
            } else {
                rows.push_back(hbox({
                    text(fit("Providers", kColName)) | bold,
                    text(fit("Key", kColKey)) | dim,
                    text(fit("Status", kColState)) | dim,
                    text(fit("Models", kColModels)) | dim,
                    text(fit("Action", kColAction)) | dim,
                }));
            }
            if (rows_container_ != nullptr) {
                rows.push_back(rows_container_->Render() | yflex);
            }

            rows.push_back(separator() | color(PANEL_BORDER));
            rows.push_back(hbox({
                section_title("Add Provider"),
                text("  pick a provider, paste your key, test, then save")
                    | dim,
            }));
            if (add_container_ != nullptr) {
                rows.push_back(hbox({
                    form_gutter("Provider"),
                    picker_area() | xflex,
                }));
                if (selected_provider_ == kCustomProviderId
                    || selected_provider_ == kLocalProviderId) {
                    rows.push_back(hbox({
                        form_gutter("Base URL"),
                        base_input_->Render() | xflex,
                    }));
                }
                rows.push_back(hbox({
                    form_gutter("API Key"),
                    key_input_->Render() | xflex,
                }));
                rows.push_back(hbox({
                    text(std::string(kFormLabel + 2, ' ')),
                    action_button_->Render(),
                    text("  "),
                    status_line_element(),
                }));
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
            ursa::resolve_modal(*state_, ModalResult {
                ModelChoice { row->connection_id, row->model_id } });
        }

        Element render_pick()
        {
            Elements rows { text("Models") | bold };
            rows.push_back(separatorEmpty());
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
        Component key_input_;
        Component action_button_;
        std::string base_buf_;
        int base_cursor_ = 0;
        std::string key_buf_;
        int key_cursor_ = 0;
        std::string tested_signature_;
        std::map<std::string, bool> confirm_;
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

} // namespace ursa
