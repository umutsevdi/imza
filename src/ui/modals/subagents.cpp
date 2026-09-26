#include "app/application_state.h"
#include "app/flows.h"
#include "providers/store.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    constexpr int SUBAGENT_ROLES = 3;

    SubagentRole role_at(int index)
    {
        if (index == 0) {
            return SubagentRole::BUILDER;
        }
        if (index == 1) {
            return SubagentRole::RESEARCH;
        }
        return SubagentRole::BASIC;
    }

    std::string role_name(SubagentRole role)
    {
        if (role == SubagentRole::BUILDER) {
            return "Builder";
        }
        if (role == SubagentRole::RESEARCH) {
            return "Research";
        }
        return "Basic";
    }

    std::string role_description(SubagentRole role)
    {
        if (role == SubagentRole::BUILDER) {
            return "edits code and runs build tasks";
        }
        if (role == SubagentRole::RESEARCH) {
            return "read-only discovery and code review";
        }
        return "small UI tasks such as session titles";
    }

    class SubagentsView : public ComponentBase {
    public:
        explicit SubagentsView(ProviderStore& providers)
            : provider_store_(providers)
        {
            pick_filter_ = Input(field_option(
                &pick_.filter, &pick_.filter_cursor, "filter models", [this] {
                    pick_.selected = 0;
                    pick_.refill_visible();
                }));
            container_   = Container::Vertical({ pick_filter_ });
        }

        Element OnRender() override
        {
            if (picking_) {
                return render_pick();
            }
            return render_roles();
        }

        bool OnEvent(Event event) override
        {
            if (picking_) {
                return handle_pick_event(event);
            }
            return handle_role_event(event);
        }

    private:
        std::string subagent_variant(SubagentRole role) const
        {
            const Config config = provider_store_.config();
            const auto found    = config.subagents.find(role);
            return to_wire_effort(subagent_variant_or_default(
                found != config.subagents.end() ? &found->second : nullptr,
                role));
        }

        void begin_pick()
        {
            pick_.rows.clear();
            pick_.rows.push_back(
                ModelRow { "", "", "<Default>", "use main chat model" });
            for (const auto& view : provider_store_.connections()) {
                const ModelList list = provider_store_.models_for(view.id);
                for (const ModelInfo& info : list.models) {
                    pick_.rows.push_back(
                        make_model_row(view.id, view.name, info));
                }
            }
            pick_.filter.clear();
            pick_.filter_cursor = 0;
            pick_.refill_visible();
            pick_.selected          = 0;
            const SubagentRole role = role_at(selected_);
            const Config config     = provider_store_.config();
            const auto found        = config.subagents.find(role);
            if (found != config.subagents.end()) {
                for (int i = 0; i < static_cast<int>(pick_.visible.size());
                    ++i) {
                    const ModelRow& row
                        = pick_
                              .rows[pick_.visible[static_cast<std::size_t>(i)]];
                    if (row.connection_id == found->second.provider
                        && row.model_id == found->second.model) {
                        pick_.selected = i;
                        break;
                    }
                }
            }
            if (pick_filter_) {
                pick_filter_->TakeFocus();
            }
            picking_ = true;
        }

        void save_model()
        {
            const ModelRow* row = pick_.chosen();
            if (!row) {
                return;
            }
            const SubagentRole role = role_at(selected_);
            provider_store_.set_subagent_model(role,
                SubagentModelConfig { row->connection_id, row->model_id,
                    subagent_variant(role) });
            picking_ = false;
        }

        void change_variant(int delta)
        {
            const SubagentRole role = role_at(selected_);
            static const std::vector<std::string> variants { "off", "low",
                "medium", "high" };
            const Config config = provider_store_.config();
            const auto found    = config.subagents.find(role);
            auto current        = std::find(
                variants.begin(), variants.end(), subagent_variant(role));
            int index = current == variants.end()
                ? 2
                : static_cast<int>(current - variants.begin());
            index     = std::clamp(
                index + delta, 0, static_cast<int>(variants.size()) - 1);
            SubagentModelConfig next;
            if (found != config.subagents.end()) {
                next = found->second;
            }
            next.variant = variants[static_cast<std::size_t>(index)];
            provider_store_.set_subagent_model(role, std::move(next));
        }

        bool handle_pick_event(const Event& event)
        {
            if (event == Event::Escape) {
                picking_ = false;
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
                save_model();
                return true;
            }
            return container_->OnEvent(event);
        }

        bool handle_role_event(const Event& event)
        {
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                move_list_cursor(event, selected_, SUBAGENT_ROLES);
                return true;
            }
            if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                change_variant(event == Event::ArrowRight ? 1 : -1);
                return true;
            }
            if (event == Event::Return) {
                begin_pick();
                return true;
            }
            return false;
        }

        Element render_pick()
        {
            Elements rows { modal_header(
                role_name(role_at(selected_)) + " Subagent Model") };
            rows.push_back(pick_filter_->Render() | xflex);
            if (pick_.visible.empty()) {
                rows.push_back(text("no matching models") | dim);
            }
            for (int i = 0; i < static_cast<int>(pick_.visible.size()); ++i) {
                const ModelRow& row
                    = pick_.rows[pick_.visible[static_cast<std::size_t>(i)]];
                rows.push_back(model_picker_row(row, i == pick_.selected));
            }
            rows.push_back(separatorEmpty());
            rows.push_back(
                hint_bar("arrows navigate · Enter select · Esc back"));
            return vbox(std::move(rows)) | xflex;
        }

        Element render_roles()
        {
            const Config config = provider_store_.config();
            Elements rows       = modal_header("Subagent Models",
                "Tune subagent tasks. Choose <Default> to follow the main "
                "chat model.");
            for (int index = 0; index < SUBAGENT_ROLES; ++index) {
                const SubagentRole role = role_at(index);
                const auto found        = config.subagents.find(role);
                std::string model       = "<Default>";
                if (found != config.subagents.end()
                    && !found->second.model.empty()) {
                    model = found->second.model;
                }
                Element row = hbox({ text(role_name(role) + " Subagent"),
                    text("  " + role_description(role)) | dim, filler(),
                    text(model),
                    text("  < " + subagent_variant(role) + " >") | dim });
                if (index == selected_) {
                    row |= bgcolor(PANEL_COLOR_FOCUS);
                    row |= bold;
                }
                rows.push_back(std::move(row));
            }
            rows.push_back(separatorEmpty());
            rows.push_back(
                hint_bar("↑↓ rows · ←→ variant · Enter model · Esc close"));
            return vbox(std::move(rows)) | xflex;
        }

        ProviderStore& provider_store_;
        ModelPickList pick_;
        Component pick_filter_;
        Component container_;
        int selected_ = 0;
        bool picking_ = false;
    };

} // namespace

ftxui::Component make_subagents(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SubagentsView>(*state->providers);
}

} // namespace imza
