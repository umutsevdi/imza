#include "pricing.h"
#include "ui.h"
#include "util.h"

#include "environment.h"

#include <cstdlib>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <sstream>
#include <string>

namespace ursa {
using namespace ftxui;

class StatusLine : public ComponentBase {
public:
    StatusLine(std::shared_ptr<ApplicationState> state, LayoutFn layout,
        WorkflowFn workflow)
        : state_(std::move(state))
        , layout_(std::move(layout))
        , workflow_(std::move(workflow))
        , workspace_subscription_(
              state_->environment->subscribe_to_workspace_change(
                  [] { animation::RequestAnimationFrame(); }))
        , repository_subscription_(
              state_->environment->subscribe_to_repository_change(
                  [] { animation::RequestAnimationFrame(); }))
        , subagent_subscription_(state_->subagents->subscribe(
              [](const SubagentEvent&) { animation::RequestAnimationFrame(); }))
    {
    }

    Element OnRender() override
    {
        using namespace ftxui;
        const StatusConfigView config     = state_->providers->status();
        const Session::StatusView session = state_->session->status_view();
        const LayoutCtx ctx               = layout_();
        const bool wide                   = ctx.kind == LayoutCtx::Kind::WIDE;
        const bool environment_ready      = state_->environment->ready();
        const WorkflowPhase phase         = workflow_();
        std::string mode_label;
        Color mode_color;
        switch (phase) {
        case WorkflowPhase::PLAN:
            mode_label = " PLAN ";
            mode_color = Color::GreenLight;
            break;
        case WorkflowPhase::BUILD:
            mode_label = " BUILD ";
            mode_color = Color::RedLight;
            break;
        case WorkflowPhase::REVIEW:
            mode_label = " REVIEW ";
            mode_color = Color::CyanLight;
            break;
        }
        Element mode = text(std::move(mode_label)) | bold
            | color(PANEL_COLOR_FOCUS) | bgcolor(mode_color);
        const std::string& active_model = config.active_model;

        const auto repository = state_->environment->repository();
        Elements bar;
        bar.push_back(text(" "));
        bar.push_back(std::move(mode));
        if (!active_model.empty()) {
            bar.push_back(text(" · " + active_model) | color(PANEL_FG_DIM));
            if (!config.reasoning_effort.empty()
                && config.reasoning_effort != "off") {
                std::string shown = config.reasoning_effort;
                if (shown == "medium") {
                    shown = "default";
                }
                const Color effort_color
                    = shown == "high" ? Color::GreenLight : PANEL_FG;
                bar.push_back(text(" (" + shown + ")") | color(effort_color));
            }
        }
        if (session.last.prompt > 0 || session.totals.total > 0) {
            const ModelPricing pricing = _cached_pricing(active_model);
            const std::uint64_t used   = session.last.prompt;
            if (pricing.context_limit > 0) {
                const std::uint64_t pct = used * 100 / pricing.context_limit;
                bar.push_back(text(" · " + compact_tokens(used) + "/"
                                  + compact_tokens(pricing.context_limit) + " ("
                                  + std::to_string(pct) + "%)")
                    | color(PANEL_FG_DIM));
            } else {
                bar.push_back(text(" · " + compact_tokens(used) + " tok")
                    | color(PANEL_FG_DIM));
            }
        }
        bar.push_back(filler());
        const std::size_t running_agents
            = state_->subagents->running_count(false);
        if (!environment_ready || running_agents > 0) {
            animation::RequestAnimationFrame();
            bar.push_back(spinner(15, static_cast<size_t>(frame_))
                | color(Color::GrayLight));
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
                bar.push_back(text(" (" + repository->branch + ")") | italic);
            }
            bar.push_back(text("  "));
        }
        if (session.total_cost > 0) {
            bar.push_back(text(money_text(session.total_cost) + "  ")
                | color(PANEL_FG_DIM));
        }
        bar.push_back(
            text(" URSA ") | bold | bgcolor(PANEL_FG) | color(PANEL_COLOR));
        return hbox(std::move(bar)) | bgcolor(PANEL_COLOR_FOCUS)
            | color(PANEL_FG) | xflex;
    }

    void OnAnimation(animation::Params&) override
    {
        if (!state_->environment->ready()
            || state_->subagents->running_count(false) > 0) {
            ++frame_;
            animation::RequestAnimationFrame();
        }
    }

private:
    std::shared_ptr<ApplicationState> state_;
    LayoutFn layout_;
    WorkflowFn workflow_;
    int frame_ = 0;
    std::string last_model_;
    ModelPricing cached_;
    Signal<>::Subscription workspace_subscription_;
    Signal<>::Subscription repository_subscription_;
    Signal<const SubagentEvent&>::Subscription subagent_subscription_;

    ModelPricing _cached_pricing(const std::string& model)
    {
        if (last_model_ != model) {
            last_model_ = model;
            cached_     = get_pricing(model);
        }
        return cached_;
    }

    std::string money_text(double cost)
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

    std::string compact_tokens(std::uint64_t n)
    {
        std::ostringstream o;
        if (n >= 1'000'000) {
            o << std::fixed << std::setprecision(1)
              << static_cast<double>(n) / 1'000'000.0 << 'M';
        } else if (n >= 1'000) {
            o << std::fixed << std::setprecision(1)
              << static_cast<double>(n) / 1'000.0 << 'k';
        } else {
            o << n;
        }
        return o.str();
    }
};

ftxui::Component make_status_line(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow)
{
    return ftxui::Make<StatusLine>(
        std::move(state), std::move(layout), std::move(workflow));
}
} // namespace ursa
