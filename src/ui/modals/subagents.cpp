#include "agent/application_state.h"
#include "agent/flows.h"
#include "provider/provider_store.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    constexpr int kSubagentRoles = 3;

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
            pick_.selected = 0;
            pick_.refill_visible();
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
            return false;
        }

        bool handle_role_event(const Event& event)
        {
            if (event == Event::ArrowDown) {
                selected_ = std::min(selected_ + 1, kSubagentRoles - 1);
                return true;
            }
            if (event == Event::ArrowUp) {
                selected_ = std::max(selected_ - 1, 0);
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
            Elements rows {
                text(role_name(role_at(selected_)) + " Subagent Model") | bold,
                separatorEmpty()
            };
            if (pick_.visible.empty()) {
                rows.push_back(text("no models available") | dim);
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
            Elements rows { text("Subagent Models") | bold,
                text("Tune subagent tasks. Choose <Default> to follow the main "
                     "chat model.")
                    | dim,
                separatorEmpty() };
            for (int index = 0; index < kSubagentRoles; ++index) {
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
        int selected_ = 0;
        bool picking_ = false;
    };

} // namespace

ftxui::Component make_subagents(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SubagentsView>(*state->providers);
}

} // namespace ursa
