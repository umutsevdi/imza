#include "app/application_state.h"
#include "common/util.h"
#include "providers/pricing.h"
#include "providers/store.h"
#include "runtime/subagent_manager.h"
#include "ui/ui.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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
            , _compaction_subscription(
                  _state->session->subscribe_to_compaction_change(
                      [] { animation::RequestAnimationFrame(); }))
        {
        }

        Element OnRender() override
        {
            const StatusConfigView config     = _state->providers->status();
            const Session::StatusView session = _state->session->status_view();
            const LayoutCtx ctx               = _layout();
            const bool wide           = ctx.kind == LayoutCtx::Kind::WIDE;
            const WorkflowPhase phase = _workflow();
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
            if (append_background_tasks(&bar, wide)) {
                animation::RequestAnimationFrame();
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

        void OnAnimation(animation::Params& params) override
        {
            if (append_background_tasks(nullptr, false)) {
                // Spinner position derives from accumulated animation time,
                // not the callback count, so its speed does not follow the
                // render loop's frame rate.
                _animation_ms
                    += std::chrono::duration_cast<std::chrono::milliseconds>(
                        params.duration());
                animation::RequestAnimationFrame();
            }
        }

    private:
        struct BackgroundTask {
            std::string label;
            bool active;
        };

        // Spinner plus the active labels; returns true when anything is
        // shown, so the caller keeps the frame loop alive. Pass nullptr
        // to only ask.
        bool append_background_tasks(Elements* bar, bool wide) const
        {
            const std::size_t agents = _state->subagents->running_count(false);
            const BackgroundTask tasks[] = {
                { "Caching…", !_state->environment->ready() },
                { "Compacting…", _state->session->compaction_running() },
                { std::to_string(agents) + (agents == 1 ? " agent" : " agents"),
                    agents > 0 },
            };
            bool any = false;
            std::string label;
            for (const BackgroundTask& task : tasks) {
                if (!task.active) {
                    continue;
                }
                if (any) {
                    label += " · ";
                }
                label += task.label;
                any = true;
            }
            if (!any || bar == nullptr) {
                return any;
            }
            bar->push_back(dim_spinner(
                static_cast<int>(_animation_ms.count() / SPINNER_FRAME_MS)));
            if (wide) {
                bar->push_back(text(" " + label) | color(PANEL_FG_DIM));
            }
            bar->push_back(text("  "));
            return true;
        }

        std::shared_ptr<ApplicationState> _state;
        LayoutFn _layout;
        WorkflowFn _workflow;
        std::chrono::milliseconds _animation_ms { };
        std::string _last_model;
        ModelPricing _cached;
        Signal<>::Subscription _workspace_subscription;
        Signal<>::Subscription _repository_subscription;
        Signal<const SubagentEvent&>::Subscription _subagent_subscription;
        Signal<>::Subscription _compaction_subscription;

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
