#include "pricing.h"
#include "ui.h"
#include "util.h"

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

std::string money_text(double cost)
{
    std::ostringstream o;
    o << '$' << std::fixed << std::setprecision(cost >= 1.0 ? 2 : 3) << cost;
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

const char* _spinner_frame()
{
    static const char* frames[]
        = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return frames[static_cast<std::size_t>(ms / 100)
        % (sizeof(frames) / sizeof(frames[0]))];
}

std::string _cached_cwd()
{
    using namespace std::chrono_literals;
    static std::string cached;
    static std::chrono::steady_clock::time_point fetched;
    const auto now = std::chrono::steady_clock::now();
    if (cached.empty() || now - fetched > 1s) {
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        cached  = ec ? "" : _abbreviate_home(cwd.string());
        fetched = now;
    }
    return cached;
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

ModelPricing _cached_pricing(const std::string& model)
{
    static std::string last_model;
    static ModelPricing cached;
    if (last_model != model) {
        last_model = model;
        cached     = get_pricing(model);
    }
    return cached;
}

Element status_line(
    const Config& cfg, const UiState& state, const LayoutCtx& ctx)
{
    const bool wide = ctx.kind == LayoutCtx::Kind::WIDE;
    const bool plan = state.mode == UiState::Mode::PLAN;
    Element mode    = text(plan ? " PLAN " : " BUILD ") | bold
        | color(PANEL_COLOR_FOCUS)
        | bgcolor(plan ? Color::GreenLight : Color::RedLight);

    const std::string active_model = cfg.last_used ? cfg.last_used->model : "";

    Elements bar;
    bar.push_back(text(" "));
    bar.push_back(std::move(mode));
    if (!active_model.empty()) {
        bar.push_back(text(" · " + active_model) | color(PANEL_FG_DIM));
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
    if (!state.env_ready) {
        bar.push_back(text(_spinner_frame()) | color(PANEL_FG_DIM));
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
        bar.push_back(
            text(money_text(state.total_cost) + "  ") | color(PANEL_FG_DIM));
    }
    bar.push_back(
        text(" URSA ") | bold | bgcolor(PANEL_FG) | color(PANEL_COLOR));
    return hbox(std::move(bar)) | bgcolor(PANEL_COLOR_FOCUS) | color(PANEL_FG)
        | xflex;
}
} // namespace ursa
