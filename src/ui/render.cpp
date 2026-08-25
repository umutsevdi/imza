#include "ui.h"

#include <ftxui/dom/elements.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    std::vector<std::string> split_text(const std::string& text)
    {
        std::vector<std::string> lines;
        std::string line;
        for (const char c : text) {
            if (c == '\n') {
                lines.push_back(std::move(line));
                line.clear();
            } else {
                line += c;
            }
        }
        lines.push_back(std::move(line));
        return lines;
    }

    std::size_t digit_width(std::size_t n)
    {
        return std::to_string(n).size();
    }

    std::string _home_dir()
    {
#ifdef _WIN32
        char* buf = nullptr;
        size_t sz = 0;
        if (_dupenv_s(&buf, &sz, "USERPROFILE") != 0 || buf == nullptr) {
            return "";
        }
        std::string value(buf);
        free(buf);
        return value;
#else
        const char* value = std::getenv("HOME");
        return value != nullptr ? std::string(value) : "";
#endif
    }

    std::string _abbreviate_home(const std::string& path)
    {
        const std::string home = _home_dir();
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

} // namespace

Element code_block(const std::string& code, const std::string& lang)
{
    const Color bg = Color::Black;
    const Color fg = Color::White;
    Elements body;
    if (!lang.empty()) {
        body.push_back(text(lang) | color(PANEL_FG_DIM));
    }
    size_t pos = 0;
    for (;;) {
        const size_t nl = code.find('\n', pos);
        const std::string line
            = code.substr(pos, nl == std::string::npos ? nl : nl - pos);
        body.push_back(text(line) | color(fg));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    Element inner = vbox(std::move(body));
    return hbox({ text(" "), std::move(inner) | xflex, text(" ") }) | bgcolor(bg);
}

Element code_block_with_lines(const std::string& code, const std::string& lang,
    std::size_t start_line)
{
    const Color bg = Color::Black;
    const Color fg = Color::White;
    const Color gutter = PANEL_FG_DIM;
    const std::vector<std::string> lines = split_text(code);

    std::size_t footer = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("[truncated", 0) == 0) {
            footer = i;
            break;
        }
    }
    const std::size_t content_end
        = footer > 0 && lines[footer - 1].empty() ? footer - 1 : footer;
    const std::size_t last_num = start_line + content_end;
    const std::size_t width    = digit_width(last_num < start_line ? start_line
                                                                    : last_num);

    Elements body;
    if (!lang.empty()) {
        body.push_back(text(lang) | color(PANEL_FG_DIM));
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i == footer) {
            body.push_back(text(lines[i]) | color(PANEL_FG_DIM));
            continue;
        }
        if (i == footer - 1 && lines[i].empty()) {
            continue;
        }
        const std::string num    = std::to_string(start_line + i);
        std::string padded(width - num.size(), ' ');
        padded += num;
        body.push_back(hbox({
            text(padded) | color(gutter),
            text(" "),
            text(lines[i]) | color(fg),
        }));
    }
    Element inner = vbox(std::move(body));
    return hbox({ text(" "), std::move(inner) | xflex, text(" ") }) | bgcolor(bg);
}

Element list_block(const std::string& code)
{
    const Color bg   = Color::Black;
    const Color fg   = PANEL_FG;
    const Color size = PANEL_FG_DIM;
    const std::vector<std::string> lines = split_text(code);

    Elements body;
    for (const std::string& raw : lines) {
        if (raw.rfind("[truncated", 0) == 0) {
            body.push_back(text(raw) | color(PANEL_FG_DIM));
            continue;
        }
        const std::size_t tab = raw.find('\t');
        const std::string name = tab == std::string::npos ? raw : raw.substr(0, tab);
        const std::string sz   = tab == std::string::npos ? "" : raw.substr(tab + 1);
        body.push_back(hbox({
            text(name) | xflex | color(fg),
            text("  "),
            text(sz) | color(size),
        }));
    }
    Element inner = vbox(std::move(body));
    return hbox({ text(" "), std::move(inner) | xflex, text(" ") }) | bgcolor(bg);
}

Element panel(Element e)
{
    return std::move(e) | bgcolor(PANEL_COLOR) | color(PANEL_FG);
}

Element card(Element body, std::optional<Color> bg)
{
    Element inner = vbox({ separatorEmpty(), std::move(body), separatorEmpty() });
    Element box = hbox({ text("  "), std::move(inner) | xflex, text("  ") });
    if (bg) {
        box = std::move(box) | bgcolor(*bg) | color(PANEL_FG);
    }
    return std::move(box) | xflex;
}

Element section_title(std::string_view title, Color fg)
{
    return text(std::string(title)) | bold | color(fg);
}

Element status_line(const Config& cfg, const UiState& state, const LayoutCtx& ctx)
{
    const bool wide = ctx.kind == LayoutCtx::Kind::WIDE;
    const bool plan = state.mode == UiState::Mode::PLAN;
    Element mode    = text(plan ? " PLAN " : " BUILD ")
        | bold | color(PANEL_COLOR_FOCUS)
        | bgcolor(plan ? Color::GreenLight : Color::RedLight);

    Elements bar;
    bar.push_back(text(" "));
    bar.push_back(std::move(mode));
    if (!cfg.model.empty()) {
        bar.push_back(text(" · " + cfg.model) | color(PANEL_FG_DIM));
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
        const std::string cwd
            = _abbreviate_home(std::filesystem::current_path().string());
        bar.push_back(text(cwd) | color(PANEL_FG_DIM));
        bar.push_back(text("  "));
    }
    bar.push_back(text("URSA") | bold | color(PANEL_FG));
    return hbox(std::move(bar)) | bgcolor(PANEL_COLOR_FOCUS)
        | color(PANEL_FG) | xflex;
}

} // namespace ursa

