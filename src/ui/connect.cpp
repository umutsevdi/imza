#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "session_store.h"
#include "util.h"

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

    std::string compact(std::uint64_t n)
    {
        if (n >= 1'000'000) {
            const double m = static_cast<double>(n) / 1'000'000.0;
            char buf[32];
            std::snprintf(buf, sizeof(buf), m >= 10 ? "%.0fM" : "%.1fM", m);
            return buf;
        }
        if (n >= 1'000) {
            const double k = static_cast<double>(n) / 1'000.0;
            char buf[32];
            std::snprintf(buf, sizeof(buf), k >= 10 ? "%.0fK" : "%.1fK", k);
            return buf;
        }
        return std::to_string(n);
    }

    bool ends_with(std::string_view text, std::string_view suffix)
    {
        return text.size() >= suffix.size()
            && text.substr(text.size() - suffix.size()) == suffix;
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

    struct ModelRow {
        std::string connection_id;
        std::string model_id;
        std::string name;
        std::string tag;
    };

    class ConnectImpl : public ComponentBase {
    public:
        explicit ConnectImpl(std::shared_ptr<Session> session,
            Controller& controller, ProviderStore& providers)
            : session_(std::move(session))
            , controller_(controller)
            , provider_store_(providers)
        {
        }

        Element OnRender() override
        {
            sync_phase();
            if (entry_ == ConnectModal::Entry::SUBAGENTS) {
                return render_subagents();
            }
            if (entry_ == ConnectModal::Entry::PICK_MODEL) {
                maybe_rebuild_pick();
                return render_pick();
            }
            maybe_rebuild_manage();
            return render_manage();
        }

        bool OnEvent(Event event) override
        {
            if (entry_ == ConnectModal::Entry::SUBAGENTS) {
                return handle_subagent_event(event);
            }
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

        static SubagentRole role_at(int index)
        {
            if (index == 0) return SubagentRole::BUILDER;
            if (index == 1) return SubagentRole::RESEARCH;
            return SubagentRole::BASIC;
        }

        static std::string role_name(SubagentRole role)
        {
            if (role == SubagentRole::BUILDER) return "Builder";
            if (role == SubagentRole::RESEARCH) return "Research";
            return "Basic";
        }

        static std::string role_description(SubagentRole role)
        {
            if (role == SubagentRole::BUILDER) {
                return "edits code and runs build tasks";
            }
            if (role == SubagentRole::RESEARCH) {
                return "read-only discovery and code review";
            }
            return "small UI tasks such as session titles";
        }

        bool handle_subagent_event(Event event)
        {
            if (subagent_picking_) {
                if (event == Event::Escape) {
                    subagent_picking_ = false;
                    return true;
                }
                if (event == Event::ArrowDown) { pick_move(1); return true; }
                if (event == Event::ArrowUp) { pick_move(-1); return true; }
                if (event == Event::Return) {
                    save_subagent_model();
                    return true;
                }
                return false;
            }
            if (event == Event::ArrowDown) {
                subagent_selected_ = std::min(subagent_selected_ + 1, 2);
                return true;
            }
            if (event == Event::ArrowUp) {
                subagent_selected_ = std::max(subagent_selected_ - 1, 0);
                return true;
            }
            if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                change_subagent_variant(event == Event::ArrowRight ? 1 : -1);
                return true;
            }
            if (event == Event::Return) {
                begin_subagent_pick();
                return true;
            }
            return false;
        }

        void begin_subagent_pick()
        {
            pick_rows_.clear();
            pick_rows_.push_back(ModelRow { "", "", "<Default>",
                "use main chat model" });
            for (const auto& view : views()) {
                const ModelList list = provider_store_.models_for(view.id);
                for (const ModelInfo& info : list.models) {
                    ModelRow row;
                    row.connection_id = view.id;
                    row.model_id      = info.id;
                    const auto slash  = info.id.find('/');
                    row.name = slash == std::string::npos
                        ? info.id : info.id.substr(slash + 1);
                    row.tag = slash == std::string::npos
                        ? view.name : info.id.substr(0, slash);
                    pick_rows_.push_back(std::move(row));
                }
            }
            pick_buf_.clear();
            pick_selected_ = 0;
            refill_pick_visible();
            subagent_picking_ = true;
        }

        std::string subagent_variant(SubagentRole role) const
        {
            const Config config = provider_store_.config();
            const auto found = config.subagents.find(role);
            if (found != config.subagents.end() && !found->second.variant.empty()) {
                return found->second.variant == "default"
                    ? "medium" : found->second.variant;
            }
            return std::string(subagent_default_variant(role));
        }

        void change_subagent_variant(int delta)
        {
            const SubagentRole role = role_at(subagent_selected_);
            static const std::vector<std::string> variants {
                "off", "low", "medium", "high" };
            const Config config = provider_store_.config();
            const auto found = config.subagents.find(role);
            auto current = std::find(
                variants.begin(), variants.end(), subagent_variant(role));
            int index = current == variants.end() ? 2
                : static_cast<int>(current - variants.begin());
            index = std::clamp(index + delta, 0,
                static_cast<int>(variants.size()) - 1);
            SubagentModelConfig next;
            if (found != config.subagents.end()) next = found->second;
            next.variant = variants[static_cast<std::size_t>(index)];
            provider_store_.set_subagent_model(role, std::move(next));
        }

        void save_subagent_model()
        {
            if (pick_visible_.empty()) return;
            const ModelRow& row = pick_rows_[pick_visible_[static_cast<std::size_t>(
                pick_selected_)]];
            const SubagentRole role = role_at(subagent_selected_);
            provider_store_.set_subagent_model(role,
                SubagentModelConfig { row.connection_id, row.model_id,
                    subagent_variant(role) });
            subagent_picking_ = false;
        }

        Element render_subagents()
        {
            if (subagent_picking_) {
                Elements rows { text(role_name(role_at(subagent_selected_))
                    + " Subagent Model") | bold, separatorEmpty() };
                if (pick_visible_.empty()) {
                    rows.push_back(text("no models available") | dim);
                }
                for (int i = 0; i < static_cast<int>(pick_visible_.size()); ++i) {
                    const ModelRow& row = pick_rows_[pick_visible_[static_cast<std::size_t>(i)]];
                    Element value = hbox({ text(i == pick_selected_ ? "› " : "  "),
                        text(row.name), filler(), text(row.tag) | dim });
                    if (i == pick_selected_) value |= bold;
                    rows.push_back(std::move(value));
                }
                rows.push_back(separatorEmpty());
                rows.push_back(hbox({ filler(),
                    text("arrows navigate · Enter select · Esc back") | dim }));
                return vbox(std::move(rows)) | xflex;
            }
            const Config config = provider_store_.config();
            Elements rows { text("Subagent Models") | bold,
                text("Tune subagent tasks. Choose <Default> to follow the main chat model.") | dim,
                separatorEmpty() };
            for (int index = 0; index < 3; ++index) {
                const SubagentRole role = role_at(index);
                const auto found = config.subagents.find(role);
                std::string model = "<Default>";
                if (found != config.subagents.end()
                    && !found->second.model.empty()) {
                    model = found->second.model;
                }
                Element row = hbox({ text(role_name(role) + " Subagent"),
                    text("  " + role_description(role)) | dim,
                    filler(), text(model),
                    text("  < " + subagent_variant(role) + " >") | dim });
                if (index == subagent_selected_) {
                    row |= bgcolor(PANEL_COLOR_FOCUS);
                    row |= bold;
                }
                rows.push_back(std::move(row));
            }
            rows.push_back(separatorEmpty());
            rows.push_back(hbox({ filler(),
                text("↑↓ rows · ←→ variant · Enter model · Esc close") | dim }));
            return vbox(std::move(rows)) | xflex;
        }

        bool handle_pick_event(Event event)
        {
            if (event == Event::F5 || event == Event::CtrlR) {
                for (const auto& view : views()) {
                    provider_store_.refetch_models(view.id);
                }
                return true;
            }
            if (event == Event::ArrowDown) {
                pick_move(1);
                return true;
            }
            if (event == Event::ArrowUp) {
                pick_move(-1);
                return true;
            }
            if (event == Event::Return) {
                submit_pick();
                return true;
            }
            return container_ ? container_->OnEvent(event) : false;
        }

        bool handle_manage_event(Event event)
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
                    subagent_picking_ = false;
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
            std::string base = strip_slash(trim(base_buf_));
            if (base.empty()) {
                return "";
            }
            return ends_with(base, "/chat/completions")
                ? base
                : base + "/chat/completions";
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
                    if (view.state == ConnectionView::State::READY) {
                        models_el = std::move(models_el) | dim;
                    } else if (view.state == ConnectionView::State::FAILED) {
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
                row_error_ = "cannot remove the last connection";
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
            controller_.resolve_modal(ModalResult { res });
        }

        void run_save()
        {
            const ConnectResult res = build_result(true);
            if (res.provider_id.empty()) {
                return;
            }
            controller_.resolve_modal(ModalResult { res });
        }

        ConnectResult build_result(bool persist)
        {
            ConnectResult res;
            res.provider_id = selected_provider_;
            res.persist     = persist;
            if (res.provider_id.empty()) {
                row_error_ = "select a provider";
                return res;
            }
            res.endpoint = current_endpoint();
            if ((res.provider_id == kCustomProviderId
                    || res.provider_id == kLocalProviderId)
                && res.endpoint.empty()) {
                row_error_      = "enter a base URL";
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
            rows.push_back(hbox({ filler(), text(hint) | dim }));
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
            pick_rows_.clear();
            pick_visible_.clear();
            for (const auto& snap : pick_snapshot_) {
                for (const ModelInfo& info : snap.list.models) {
                    ModelRow row;
                    row.connection_id = snap.view.id;
                    row.model_id      = info.id;
                    const auto slash  = info.id.find('/');
                    if (slash == std::string::npos) {
                        row.name = info.id;
                        row.tag  = snap.view.name;
                    } else {
                        row.name = info.id.substr(slash + 1);
                        row.tag  = info.id.substr(0, slash);
                    }
                    if (info.context_length && *info.context_length > 0) {
                        row.tag += " · " + compact(*info.context_length);
                    }
                    pick_rows_.push_back(std::move(row));
                }
            }
            const Config cfg = provider_store_.config();
            if (cfg.last_used && !cfg.last_used->model.empty()) {
                for (std::size_t i = 0; i < pick_rows_.size(); ++i) {
                    ModelRow& row = pick_rows_[i];
                    if (row.connection_id == cfg.last_used->provider
                        && row.model_id == cfg.last_used->model) {
                        row.tag += " · current";
                        std::rotate(pick_rows_.begin(),
                            pick_rows_.begin() + static_cast<std::ptrdiff_t>(i),
                            pick_rows_.end());
                        break;
                    }
                }
            }
            pick_selected_ = 0;
            refill_pick_visible();

            pick_filter_ = Input(field_option(
                &pick_buf_, &pick_cursor_, "filter models", [this] {
                    pick_selected_ = 0;
                    refill_pick_visible();
                }));

            container_ = Container::Vertical({ pick_filter_ });
        }

        void refill_pick_visible()
        {
            const std::string needle = to_lower(trim(pick_buf_));
            pick_visible_.clear();
            for (std::size_t i = 0; i < pick_rows_.size(); ++i) {
                if (!needle.empty()
                    && to_lower(pick_rows_[i].model_id).find(needle)
                        == std::string::npos
                    && to_lower(pick_rows_[i].name).find(needle)
                        == std::string::npos) {
                    continue;
                }
                pick_visible_.push_back(i);
            }
        }

        void pick_move(int delta)
        {
            if (pick_visible_.empty()) {
                return;
            }
            pick_selected_ = std::clamp(pick_selected_ + delta, 0,
                static_cast<int>(pick_visible_.size()) - 1);
        }

        void submit_pick()
        {
            if (pick_visible_.empty()
                || pick_selected_ >= static_cast<int>(pick_visible_.size())) {
                return;
            }
            const ModelRow& row
                = pick_rows_[pick_visible_[static_cast<std::size_t>(
                    pick_selected_)]];
            controller_.resolve_modal(ModalResult {
                ModelChoice { row.connection_id, row.model_id } });
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

            if (pick_visible_.empty()) {
                if (!any_fetching) {
                    rows.push_back(any_failed
                            ? status_element(
                                  "✗ some providers failed — press F5 to retry",
                                  false)
                            : text("no models") | dim);
                }
            } else {
                for (int i = 0; i < static_cast<int>(pick_visible_.size());
                    ++i) {
                    const bool selected = i == pick_selected_;
                    const ModelRow& row
                        = pick_rows_[pick_visible_[static_cast<std::size_t>(
                            i)]];
                    Element e = hbox({
                        text(selected ? "› " : "  "),
                        text(row.name),
                        filler(),
                        text(row.tag) | dim,
                    });
                    if (selected) {
                        e = std::move(e) | bold;
                    }
                    rows.push_back(std::move(e));
                }
            }

            rows.push_back(separatorEmpty());
            rows.push_back(hbox({ filler(),
                text("Enter pick · F5 refresh · Esc close") | dim }));
            return vbox(std::move(rows)) | xflex;
        }

        std::shared_ptr<Session> session_;
        Controller& controller_;
        ProviderStore& provider_store_;
        ConnectModal::Entry entry_ = ConnectModal::Entry::MANAGE;
        int subagent_selected_ = 0;
        bool subagent_picking_ = false;

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
        std::vector<ModelRow> pick_rows_;
        std::vector<std::size_t> pick_visible_;
        int pick_selected_ = 0;
        Component pick_filter_;
        std::string pick_buf_;
        int pick_cursor_ = 0;
    };

} // namespace

ftxui::Component make_connect(std::shared_ptr<Session> session,
    Controller& controller, ProviderStore& providers)
{
    return ftxui::Make<ConnectImpl>(std::move(session), controller, providers);
}

void print_session_saved_box()
{
    using namespace ftxui;
    const int term_w = Terminal::Size().dimx;
    const int width  = std::max(40, std::min(term_w, 80));

    std::vector<SavedSession> sessions = saved_sessions();
    if (static_cast<int>(sessions.size()) > 5) {
        sessions.resize(5);
    }

    const int inner  = width - 2;
    const int body_w = std::max(inner, 48);

    Elements rows;
    if (sessions.empty()) {
        rows.push_back(text("No other saved sessions.") | dim);
    } else {
        rows.push_back(text("Previous Sessions") | bold);
        const int stamp_col = 16;
        const int title_col = std::max(body_w - 2 - stamp_col, 1);
        for (const auto& session : sessions) {
            const std::string title
                = session.title.empty() ? "Untitled session" : session.title;
            rows.push_back(
                hbox({ text(fit(title, title_col)) | color(PANEL_FG) | xflex,
                    text(session.saved_at) | color(PANEL_FG_DIM) }));
        }
    }

    const int height = static_cast<int>(rows.size() + 6);
    Element frame    = vbox({
        text("Session has been saved."),
        text("Continue with /session next time you launch.") | dim | italic,
        separatorEmpty(),
        vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER)
            | bgcolor(PANEL_COLOR) | color(PANEL_FG),
    });

    auto screen
        = Screen::Create(Dimension::Fixed(body_w), Dimension::Fixed(height));
    Render(screen, frame);
    std::cout << screen.ToString() << std::endl;
}

} // namespace ursa
