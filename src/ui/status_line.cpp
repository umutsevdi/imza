#include "app/application_state.h"
#include "common/util.h"
#include "providers/pricing.h"
#include "providers/store.h"
#include "runtime/subagent_manager.h"
#include "ui/ui.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace imza {
using namespace ftxui;

namespace {

    class StatusLine : public ComponentBase {
    public:
        StatusLine(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            WorkflowFn workflow)
            : _state(std::move(state))
            , _layout(std::move(layout))
            , _workflow(std::move(workflow))
            , _workspace_subscription(
                  _state->environment->subscribe_to_workspace_change(
                      [] { animation::RequestAnimationFrame(); }))
            , _repository_subscription(
                  _state->environment->subscribe_to_repository_change(
                      [] { animation::RequestAnimationFrame(); }))
            , _subagent_subscription(
                  _state->subagents->subscribe([](const SubagentEvent&) {
                      animation::RequestAnimationFrame();
                  }))
        {
        }

        Element OnRender() override
        {
            const StatusConfigView config     = _state->providers->status();
            const Session::StatusView session = _state->session->status_view();
            const LayoutCtx ctx               = _layout();
            const bool wide              = ctx.kind == LayoutCtx::Kind::WIDE;
            const bool environment_ready = _state->environment->ready();
            const WorkflowPhase phase    = _workflow();
            std::string mode_label;
            Color mode_color;
            switch (phase) {
            case WorkflowPhase::PLAN:
                mode_label = " PLAN ";
                mode_color = HL_GREEN;
                break;
            case WorkflowPhase::BUILD:
                mode_label = " BUILD ";
                mode_color = HL_RED;
                break;
            case WorkflowPhase::REVIEW:
                mode_label = " REVIEW ";
                mode_color = HL_CYAN;
                break;
            }
            Element mode = text(std::move(mode_label)) | bold
                | color(PANEL_COLOR_FOCUS) | bgcolor(mode_color);
            const std::string& active_model = config.active_model;

            const auto repository = _state->environment->repository();
            Elements bar;
            bar.push_back(text(" "));
            bar.push_back(std::move(mode));
            if (!active_model.empty()) {
                bar.push_back(text(" · " + active_model) | color(PANEL_FG_DIM));
                if (!config.reasoning_effort.empty()
                    && config.reasoning_effort != "off") {
                    const std::string shown
                        = to_config_effort(config.reasoning_effort);
                    const Color effort_color
                        = shown == "high" ? HL_GREEN : PANEL_FG;
                    bar.push_back(
                        text(" (" + shown + ")") | color(effort_color));
                }
            }
            if (session.last.prompt > 0 || session.totals.total > 0) {
                const ModelPricing pricing = _cached_pricing(active_model);
                const std::uint64_t used   = session.last.prompt;
                if (pricing.context_limit > 0) {
                    const std::uint64_t pct
                        = used * 100 / pricing.context_limit;
                    bar.push_back(text(" · " + compact_number(used) + "/"
                                      + compact_number(pricing.context_limit)
                                      + " (" + std::to_string(pct) + "%)")
                        | color(PANEL_FG_DIM));
                } else {
                    bar.push_back(text(" · " + compact_number(used) + " tok")
                        | color(PANEL_FG_DIM));
                }
            }
            if (session.total_cost > 0) {
                bar.push_back(text(" · " + _money_text(session.total_cost))
                    | color(PANEL_FG_DIM));
            }
            const std::string tags = capability_tags(_active_capabilities());
            if (!tags.empty()) {
                bar.push_back(text(" · " + tags) | color(PANEL_FG_DIM));
            }
            bar.push_back(filler());
            const std::size_t running_agents
                = _state->subagents->running_count(false);
            if (!environment_ready || running_agents > 0) {
                animation::RequestAnimationFrame();
                bar.push_back(dim_spinner(_frame));
                if (wide) {
                    bar.push_back(!environment_ready
                            ? text(" Caching…")
                            : text(" " + std::to_string(running_agents)
                                  + (running_agents == 1 ? " agent  "
                                                         : " agents  "))
                                | color(PANEL_FG_DIM));
                }
                bar.push_back(text("  "));
            }
            if (wide) {
                bar.push_back(text(_cwd()) | color(PANEL_FG_DIM));
                if (repository && !repository->branch.empty()) {
                    bar.push_back(
                        text(" (" + repository->branch + ")") | italic);
                }
                bar.push_back(text("  "));
            }
            bar.push_back(text(wide ? " IMZA v" IMZA_VERSION " " : " IMZA ")
                | bold | bgcolor(PANEL_FG) | color(PANEL_COLOR));
            return hbox(std::move(bar)) | bgcolor(PANEL_COLOR_FOCUS)
                | color(PANEL_FG) | xflex;
        }

        void OnAnimation(animation::Params&) override
        {
            if (!_state->environment->ready()
                || _state->subagents->running_count(false) > 0) {
                ++_frame;
                animation::RequestAnimationFrame();
            }
        }

    private:
        std::shared_ptr<ApplicationState> _state;
        LayoutFn _layout;
        WorkflowFn _workflow;
        int _frame = 0;
        std::string _last_model;
        ModelPricing _cached;
        Signal<>::Subscription _workspace_subscription;
        Signal<>::Subscription _repository_subscription;
        Signal<const SubagentEvent&>::Subscription _subagent_subscription;

        ModelPricing _cached_pricing(const std::string& model)
        {
            if (_last_model != model) {
                _last_model = model;
                _cached     = _state->providers->pricing_for(model);
            }
            return _cached;
        }

        std::optional<Capabilities> _active_capabilities()
        {
            const auto selection = _state->providers->active_selection();
            if (!selection) {
                return std::nullopt;
            }
            const ModelList models
                = _state->providers->models_for(selection->connection_id);
            if (models.state != ModelList::State::READY) {
                return std::nullopt;
            }
            const auto model = std::find_if(models.models.begin(),
                models.models.end(), [&](const ModelInfo& info) {
                    return info.id == selection->model;
                });
            return model == models.models.end() ? std::nullopt
                                                : model->capabilities;
        }

        std::string _money_text(double cost)
        {
            std::ostringstream o;
            o << '$' << std::fixed << std::setprecision(cost >= 1.0 ? 2 : 3)
              << cost;
            return o.str();
        }

        std::string _abbreviate_home(const std::string& path)
        {
            const std::string home = home_dir();
            if (home.empty()) {
                return path;
            }
            if (path == home) {
                return "~";
            }
            if (path.rfind(home + "/", 0) == 0) {
                return "~" + path.substr(home.size());
            }
            return path;
        }

        std::string _cwd()
        {
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            return ec ? "" : _abbreviate_home(cwd.string());
        }
    };

} // namespace

ftxui::Component make_status_line(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow)
{
    return ftxui::Make<StatusLine>(
        std::move(state), std::move(layout), std::move(workflow));
}
} // namespace imza
