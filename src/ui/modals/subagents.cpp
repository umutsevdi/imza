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
            : _provider_store(providers)
        {
            _pick_filter = make_model_pick_filter(_pick);
            _container   = Container::Vertical({ _pick_filter });
        }

        Element OnRender() override
        {
            if (_picking) {
                return _render_pick();
            }
            return _render_roles();
        }

        bool OnEvent(Event event) override
        {
            if (_picking) {
                return _handle_pick_event(event);
            }
            return _handle_role_event(event);
        }

    private:
        std::string _subagent_variant(SubagentRole role) const
        {
            const Config config = _provider_store.config();
            const auto found    = config.subagents.find(role);
            return to_wire_effort(subagent_variant_or_default(
                found != config.subagents.end() ? &found->second : nullptr,
                role));
        }

        void _begin_pick()
        {
            _pick.rows.clear();
            _pick.rows.push_back(
                ModelRow { "", "", "<Default>", "use main chat model" });
            for (const auto& view : _provider_store.connections()) {
                const ModelList list = _provider_store.models_for(view.id);
                for (const ModelInfo& info : list.models) {
                    _pick.rows.push_back(
                        make_model_row(view.id, view.name, info));
                }
            }
            _pick.filter.clear();
            _pick.filter_cursor = 0;
            _pick.refill_visible();
            _pick.selected          = 0;
            const SubagentRole role = role_at(_selected);
            const Config config     = _provider_store.config();
            const auto found        = config.subagents.find(role);
            if (found != config.subagents.end()) {
                for (int i = 0; i < static_cast<int>(_pick.visible.size());
                    ++i) {
                    const ModelRow& row
                        = _pick
                              .rows[_pick.visible[static_cast<std::size_t>(i)]];
                    if (row.connection_id == found->second.provider
                        && row.model_id == found->second.model) {
                        _pick.selected = i;
                        break;
                    }
                }
            }
            if (_pick_filter) {
                _pick_filter->TakeFocus();
            }
            _picking = true;
        }

        void _save_model()
        {
            const ModelRow* row = _pick.chosen();
            if (!row) {
                return;
            }
            const SubagentRole role = role_at(_selected);
            _provider_store.set_subagent_model(role,
                SubagentModelConfig { row->connection_id, row->model_id,
                    _subagent_variant(role) });
            _picking = false;
        }

        void _change_variant(int delta)
        {
            const SubagentRole role = role_at(_selected);
            static const std::vector<std::string> variants { "off", "low",
                "medium", "high" };
            const Config config = _provider_store.config();
            const auto found    = config.subagents.find(role);
            auto current        = std::find(
                variants.begin(), variants.end(), _subagent_variant(role));
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
            _provider_store.set_subagent_model(role, std::move(next));
        }

        bool _handle_pick_event(const Event& event)
        {
            if (event == Event::Escape) {
                _picking = false;
                return true;
            }
            return handle_model_pick_event(
                _pick, _container, event, [this] { _save_model(); });
        }

        bool _handle_role_event(const Event& event)
        {
            if (event == Event::ArrowDown || event == Event::ArrowUp) {
                move_list_cursor(event, _selected, SUBAGENT_ROLES);
                return true;
            }
            if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                _change_variant(event == Event::ArrowRight ? 1 : -1);
                return true;
            }
            if (event == Event::Return) {
                _begin_pick();
                return true;
            }
            return false;
        }

        Element _render_pick()
        {
            return render_model_pick(
                role_name(role_at(_selected)) + " Subagent Model", _pick_filter,
                _pick, nullptr, text("no matching models") | dim,
                "arrows navigate · Enter select · Esc back");
        }

        Element _render_roles()
        {
            const Config config = _provider_store.config();
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
                    text("  < " + _subagent_variant(role) + " >") | dim });
                if (index == _selected) {
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

        ProviderStore& _provider_store;
        ModelPickList _pick;
        Component _pick_filter;
        Component _container;
        int _selected = 0;
        bool _picking = false;
    };

} // namespace

ftxui::Component make_subagents(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<SubagentsView>(*state->providers);
}

} // namespace imza
