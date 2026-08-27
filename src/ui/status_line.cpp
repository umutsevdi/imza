#include "pricing.h"
#include "ui.h"
#include "util.h"

#include "environment.h"

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

namespace ursa {
using namespace ftxui;

class StatusLine : public ComponentBase {
public:
    StatusLine(std::shared_ptr<Session> session, Controller& controller,
            LayoutFn layout)
            : session_(std::move(session))
            , controller_(controller)
            , layout_(std::move(layout)) { };

    Element OnRender() override
    {
        using namespace ftxui;
        const StatusConfigView config = controller_.status_config();
        const Session::StatusView state = session_->status_view();
        const LayoutCtx ctx = layout_();
        const bool wide              = ctx.kind == LayoutCtx::Kind::WIDE;
        const bool plan              = state.mode == Session::Mode::PLAN;
        const bool environment_ready = get_environment()->ready();
        Element mode = text(plan ? " PLAN " : " BUILD ") | bold
            | color(PANEL_COLOR_FOCUS)
            | bgcolor(plan ? Color::GreenLight : Color::RedLight);
        const std::string& active_model = config.active_model;

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
        if (state.last.prompt > 0 || state.totals.total > 0) {
            const ModelPricing pricing = _cached_pricing(active_model);
            const std::uint64_t used   = state.last.prompt;
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
        if (!environment_ready) {
            animation::RequestAnimationFrame();
            bar.push_back(spinner(15, static_cast<size_t>(frame_))
                | color(Color::GrayLight));
            if (wide) {
                bar.push_back(text(" Caching…") | color(PANEL_FG_DIM));
            }
            bar.push_back(text("  "));
        }
        if (wide) {
            bar.push_back(text(_cached_cwd()) | color(PANEL_FG_DIM));
            bar.push_back(text("  "));
        }
        if (state.total_cost > 0) {
            bar.push_back(text(money_text(state.total_cost) + "  ")
                | color(PANEL_FG_DIM));
        }
        bar.push_back(
            text(" URSA ") | bold | bgcolor(PANEL_FG) | color(PANEL_COLOR));
        return hbox(std::move(bar)) | bgcolor(PANEL_COLOR_FOCUS)
            | color(PANEL_FG) | xflex;
    }

    void OnAnimation(animation::Params&) override
    {
        if (!get_environment()->ready()) {
            ++frame_;
        }
    }

private:
    std::shared_ptr<Session> session_;
    Controller& controller_;
    LayoutFn layout_;
    int frame_ = 0;
    std::string last_model_;
    ModelPricing cached_;
    std::string cached_pwd;
    std::chrono::steady_clock::time_point fetched_;

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

    std::string _cached_cwd()
    {
        using namespace std::chrono_literals;
        const auto now = std::chrono::steady_clock::now();
        if (cached_pwd.empty() || now - fetched_ > 1s) {
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            cached_pwd = ec ? "" : _abbreviate_home(cwd.string());
            fetched_   = now;
        }
        return cached_pwd;
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

ftxui::Component make_status_line(
    std::shared_ptr<Session> session, Controller& controller, LayoutFn layout)
{
    return ftxui::Make<StatusLine>(
        std::move(session), controller, std::move(layout));
}
} // namespace ursa
