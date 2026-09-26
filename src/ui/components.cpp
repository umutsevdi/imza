#include "common/util.h"
#include "ui/ui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <functional>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace imza {

ModelRow make_model_row(const std::string& connection_id,
    const std::string& provider_name, const ModelInfo& info)
{
    ModelRow row;
    row.connection_id = connection_id;
    row.model_id      = info.id;
    const auto slash  = info.id.find('/');
    row.name = slash == std::string::npos ? info.id : info.id.substr(slash + 1);
    row.tag
        = slash == std::string::npos ? provider_name : info.id.substr(0, slash);
    row.capabilities = info.capabilities;
    return row;
}

std::string capability_tags(const std::optional<Capabilities>& capabilities)
{
    if (!capabilities) {
        return "";
    }
    const bool image = has_capability(*capabilities, Capabilities::IMAGE);
    const bool pdf   = has_capability(*capabilities, Capabilities::PDF);
    if (image && pdf) {
        return "image · pdf";
    }
    return image ? "image" : pdf ? "pdf" : "";
}

ftxui::Element model_picker_row(const ModelRow& row, bool selected)
{
    std::vector<ftxui::Element> columns {
        ftxui::text(selected ? "› " : "  "),
        ftxui::text(row.name),
        ftxui::filler(),
    };
    const std::string tags = capability_tags(row.capabilities);
    if (!tags.empty()) {
        columns.push_back(ftxui::text(tags + " · ") | ftxui::dim);
    }
    columns.push_back(ftxui::text(row.tag) | ftxui::dim);
    ftxui::Element e = ftxui::hbox(std::move(columns));
    if (selected) {
        e = std::move(e) | ftxui::bold;
    }
    return e;
}

std::vector<std::size_t> filter_visible(const std::string& filter,
    std::size_t row_count, const std::function<std::string(std::size_t)>& match)
{
    const std::string needle = to_lower(trim(filter));
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < row_count; ++i) {
        if (!needle.empty()
            && to_lower(match(i)).find(needle) == std::string::npos) {
            continue;
        }
        visible.push_back(i);
    }
    return visible;
}

void ModelPickList::refill_visible()
{
    visible = filter_visible(filter, rows.size(), [this](std::size_t i) {
        return rows[i].model_id + ' ' + rows[i].name;
    });
}

void ModelPickList::move(int delta)
{
    if (visible.empty()) {
        return;
    }
    selected
        = std::clamp(selected + delta, 0, static_cast<int>(visible.size()) - 1);
}

const ModelRow* ModelPickList::chosen() const
{
    if (visible.empty() || selected >= static_cast<int>(visible.size())) {
        return nullptr;
    }
    return &rows[visible[static_cast<std::size_t>(selected)]];
}

std::string compact_number(std::uint64_t n)
{
    const auto scaled
        = [n](double divisor) { return static_cast<double>(n) / divisor; };
    char buf[32];
    if (n >= 1'000'000) {
        const double m = scaled(1'000'000.0);
        std::snprintf(buf, sizeof(buf), m >= 10 ? "%.0fM" : "%.1fM", m);
        return buf;
    }
    if (n >= 1'000) {
        const double k = scaled(1'000.0);
        std::snprintf(buf, sizeof(buf), k >= 10 ? "%.0fK" : "%.1fK", k);
        return buf;
    }
    return std::to_string(n);
}

std::string choice_marker(bool multi, bool selected)
{
    return multi ? (selected ? "▣ " : "☐ ") : (selected ? "◉ " : "○ ");
}

ftxui::Element hint_bar(std::string hint)
{
    return ftxui::hbox(
        { ftxui::filler(), ftxui::text(std::move(hint)) | ftxui::dim });
}

ftxui::Element choice_label(std::string label, bool selected, bool focused)
{
    ftxui::Element e = ftxui::text(std::move(label));
    if (focused) {
        e = std::move(e) | ftxui::bold | ftxui::color(PANEL_FG)
            | ftxui::inverted;
    } else if (selected) {
        e = std::move(e) | ftxui::bold | ftxui::color(PANEL_FG);
    } else {
        e = std::move(e) | ftxui::color(PANEL_FG_DIM);
    }
    return e;
}

bool move_list_cursor(const ftxui::Event& event, int& cursor, int count)
{
    if (event == ftxui::Event::ArrowDown && cursor + 1 < count) {
        ++cursor;
        return true;
    }
    if (event == ftxui::Event::ArrowUp && cursor > 0) {
        --cursor;
        return true;
    }
    return false;
}

namespace {
    int glyph_width(std::string_view glyph)
    {
        const int width = ftxui::string_width(std::string(glyph));
        return width < 1 ? 1 : width;
    }
} // namespace

std::string fit(const std::string& value, int width)
{
    const std::size_t max = static_cast<std::size_t>(std::max(width, 0));
    std::string out;
    std::size_t seen = 0;
    std::size_t i    = 0;
    while (i < value.size() && seen < max) {
        const auto lead = static_cast<unsigned char>(value[i]);
        const std::size_t length
            = std::min(utf8_sequence_length(lead), value.size() - i);
        if (seen + 1 == max && i + length < value.size()) {
            return out + "…";
        }
        out.append(value, i, length);
        ++seen;
        i += length;
    }
    out.append(max - seen, ' ');
    return out;
}

std::vector<std::pair<std::size_t, std::size_t>> wrap_row_ranges(
    std::string_view line, int width)
{
    const std::size_t max = static_cast<std::size_t>(std::max(width, 1));
    std::vector<std::pair<std::size_t, std::size_t>> rows;
    std::size_t row_begin = 0;
    std::size_t row_width = 0;
    std::size_t break_at  = std::string_view::npos;
    std::size_t i         = 0;
    while (i < line.size()) {
        const std::size_t length
            = std::min(utf8_sequence_length(line[i]), line.size() - i);
        const std::size_t glyph
            = static_cast<std::size_t>(glyph_width(line.substr(i, length)));
        if (row_width + glyph > max && i > row_begin) {
            if (break_at != std::string_view::npos && break_at > row_begin) {
                rows.emplace_back(row_begin, break_at);
                i = break_at;
            } else {
                rows.emplace_back(row_begin, i);
            }
            row_begin = i;
            row_width = 0;
            break_at  = std::string_view::npos;
            continue;
        }
        row_width += glyph;
        if (line[i] == ' ') {
            break_at = i + 1;
        }
        i += length;
    }
    rows.emplace_back(row_begin, line.size());
    return rows;
}
std::vector<std::string> wrap_text(std::string_view body, int width)
{
    std::vector<std::string> out;
    std::size_t line_begin = 0;
    while (line_begin <= body.size()) {
        const std::size_t newline = body.find('\n', line_begin);
        const std::size_t line_end
            = newline == std::string_view::npos ? body.size() : newline;
        const std::string_view line
            = body.substr(line_begin, line_end - line_begin);
        for (const auto& [begin, end] : wrap_row_ranges(line, width)) {
            out.emplace_back(line.substr(begin, end - begin));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        line_begin = newline + 1;
    }
    return out;
}

ftxui::Element wrapped_input_element(std::string_view content,
    std::size_t cursor, int width, std::string_view placeholder, bool focused)
{
    using namespace ftxui;

    if (content.empty()) {
        return text(placeholder) | dim;
    }

    cursor = std::min(cursor, content.size());
    Elements rows;
    std::size_t line_begin = 0;
    while (line_begin <= content.size()) {
        const std::size_t newline = content.find('\n', line_begin);
        const std::size_t line_end
            = newline == std::string_view::npos ? content.size() : newline;
        const std::string_view line
            = content.substr(line_begin, line_end - line_begin);
        for (const auto& [begin, end] : wrap_row_ranges(line, width)) {
            const std::size_t row_begin = line_begin + begin;
            const std::size_t row_end   = line_begin + end;
            const bool cursor_here      = cursor >= row_begin
                && (cursor < row_end
                    || (cursor == row_end && end == line.size()));
            if (!cursor_here) {
                rows.push_back(text(line.substr(begin, end - begin)));
                continue;
            }

            const std::size_t local_cursor = cursor - line_begin;
            const std::string_view before
                = line.substr(begin, local_cursor - begin);
            const std::size_t glyph_end = local_cursor < line.size()
                ? std::min(line.size(),
                      local_cursor
                          + utf8_sequence_length(
                              static_cast<unsigned char>(line[local_cursor])))
                : local_cursor;
            Element cursor_cell         = text(local_cursor < line.size()
                    ? line.substr(local_cursor, glyph_end - local_cursor)
                    : std::string_view(" "));
            if (focused) {
                cursor_cell = std::move(cursor_cell) | focusCursorBarBlinking;
            }
            rows.push_back(hbox({ text(before), std::move(cursor_cell),
                text(line.substr(glyph_end, end - glyph_end)) }));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        line_begin = newline + 1;
    }
    return vbox(std::move(rows));
}

LayoutCtx layout_context(int width, int height)
{
    return { width >= LayoutCtx::WIDE_THRESHOLD ? LayoutCtx::Kind::WIDE
                                                : LayoutCtx::Kind::NARROW,
        width, height };
}

using namespace ftxui;

namespace {

    std::string error_sentence(std::string message)
    {
        std::string trimmed(trim(message));
        if (trimmed.empty()) {
            return trimmed;
        }
        trimmed.front() = static_cast<char>(
            std::toupper(static_cast<unsigned char>(trimmed.front())));
        return ensure_sentence_end(std::move(trimmed));
    }

} // namespace

Element session_error_element(const Session& session)
{
    std::string message = error_sentence(session.error());
    if (session.retry_countdown()) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            session.retry_countdown()->deadline
            - std::chrono::steady_clock::now())
                             .count();
        if (remaining < 0) {
            remaining = 0;
        }
        message = session.retry_countdown()->stalled
            ? "Connection stalled - retrying in " + std::to_string(remaining)
                + "s…"
            : "Rate limited - retrying in " + std::to_string(remaining) + "s…";
    }
    if (message.empty()) {
        return text("");
    }
    return hbox({
        text(" ") | bgcolor(HL_RED),
        text(" " + message) | bgcolor(HL_RED),
        filler() | bgcolor(HL_RED),
    });
}

namespace {
    std::size_t digit_width(std::size_t n) { return std::to_string(n).size(); }

    Element code_block_frame(Elements body, const Color& bg, bool highlighted)
    {
        Element inner = vbox(std::move(body));
        Element block = hbox({ text(" "), std::move(inner) | xflex, text(" ") })
            | bgcolor(bg);
        return highlighted ? std::move(block) : std::move(block) | dim;
    }

    // Visual rows of `content` wrapped to `width`; highlighting runs on the
    // full line so tokens keep their color across the wrap.
    Elements wrapped_content_rows(
        const std::string& content, const std::string& syntax, int width)
    {
        const std::vector<std::string> segments = wrap_text(content, width);
        Elements highlighted;
        for (std::vector<Element>& rows :
            highlight_code_wrapped(content, syntax, width)) {
            std::move(
                rows.begin(), rows.end(), std::back_inserter(highlighted));
        }
        Elements out;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            out.push_back(i < highlighted.size()
                    ? std::move(highlighted[i])
                    : text(segments[i]) | color(PANEL_FG));
        }
        return out;
    }
} // namespace

Element code_block(const std::string& code, const std::string& lang, int width)
{
    const Color bg = PANEL_COLOR;
    const Color fg = PANEL_FG;
    Elements body;
    if (!lang.empty()) {
        body.push_back(text(lang) | color(PANEL_FG_DIM));
    }
    const int content_width = std::max(1, width - 4);
    if (syntax_type_supported(lang)) {
        for (std::vector<Element>& rows :
            highlight_code_wrapped(code, lang, content_width)) {
            std::move(rows.begin(), rows.end(), std::back_inserter(body));
        }
    } else {
        for (const std::string& segment : wrap_text(code, content_width)) {
            body.push_back(text(segment) | color(fg));
        }
    }
    return code_block_frame(std::move(body), bg, syntax_type_supported(lang));
}

Element code_block_with_lines(const std::string& code, const std::string& lang,
    std::size_t start_line, int width)
{
    const Color bg                       = PANEL_COLOR;
    const Color fg                       = PANEL_FG;
    const Color gutter                   = PANEL_FG_DIM;
    const std::vector<std::string> lines = split_lines(code);

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
    const std::size_t number_size
        = digit_width(last_num < start_line ? start_line : last_num);
    const int content_width
        = std::max(1, width - static_cast<int>(number_size) - 5);
    std::vector<std::vector<Element>> highlighted;
    if (syntax_type_supported(lang)) {
        highlighted = highlight_code_wrapped(code, lang, content_width);
    }

    Elements body;
    if (!lang.empty()) {
        body.push_back(text(lang) | color(PANEL_FG_DIM));
    }
    static const Elements empty_rows;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i == footer) {
            body.push_back(text(lines[i]) | color(PANEL_FG_DIM));
            continue;
        }
        if (i == footer - 1 && lines[i].empty()) {
            continue;
        }
        const std::string num = std::to_string(start_line + i);
        std::string padded(number_size - num.size(), ' ');
        padded += num;
        const std::string blank_gutter(number_size, ' ');
        const std::vector<std::string> segments
            = wrap_text(lines[i], content_width);
        const std::vector<Element>& highlighted_rows
            = i < highlighted.size() ? highlighted[i] : empty_rows;
        for (std::size_t j = 0; j < segments.size(); ++j) {
            body.push_back(hbox({
                j == 0 ? text(padded) | color(gutter) : text(blank_gutter),
                text(" "),
                j < highlighted_rows.size() ? highlighted_rows[j]
                                            : text(segments[j]) | color(fg),
            }));
        }
    }
    return code_block_frame(std::move(body), bg, syntax_type_supported(lang));
}

Elements modal_header(std::string title, std::string subtitle)
{
    Elements rows;
    rows.push_back(text(std::move(title)) | bold);
    if (!subtitle.empty()) {
        rows.push_back(text(std::move(subtitle)) | dim);
    }
    rows.push_back(separatorEmpty());
    return rows;
}

Element panel(Element e)
{
    return std::move(e) | bgcolor(PANEL_COLOR) | color(PANEL_FG);
}

Component space_activates(Component child, std::function<void()> on_space)
{
    return CatchEvent(std::move(child),
        [on_space = std::move(on_space)](const Event& e) -> bool {
            if (e == Event::Character(' ')) {
                on_space();
                return true;
            }
            return false;
        });
}

InputOption field_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change,
    std::function<void()> on_enter)
{
    InputOption io;
    io.content         = content;
    io.cursor_position = cursor;
    io.placeholder     = std::move(placeholder);
    io.multiline       = false;
    io.on_change       = std::move(on_change);
    io.on_enter        = std::move(on_enter);
    io.transform       = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        state.element |= underlined;
        state.element
            |= bgcolor(state.focused ? PANEL_COLOR_FOCUS : PANEL_COLOR);
        return state.element;
    };
    return io;
}

InputOption password_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change)
{
    InputOption io = field_option(
        content, cursor, std::move(placeholder), std::move(on_change));
    io.password = true;
    return io;
}

InputOption multiline_field_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change)
{
    InputOption option = field_option(
        content, cursor, std::move(placeholder), std::move(on_change));
    option.multiline = true;
    option.transform = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim | color(PANEL_FG_DIM);
        } else {
            state.element |= color(PANEL_FG);
        }
        state.element
            |= bgcolor(state.focused ? PANEL_COLOR_FOCUS : PANEL_COLOR);
        return state.element;
    };
    return option;
}

Component action_button(std::string label, std::function<void()> on_click,
    const Color& color_bg, const Color& color_focussed)
{
    ButtonOption bo;
    bo.transform = [label, color_focussed, color_bg](const EntryState& state) {
        Element e = text(" " + label + " ");
        if (state.focused) {
            e = std::move(e) | bold | bgcolor(color_focussed) | color(PANEL_FG);
        } else {
            e = std::move(e) | bgcolor(color_bg) | color(PANEL_FG);
        }
        return e;
    };
    return space_activates(Button(std::move(label), on_click, bo), on_click);
}

Component inline_link_button(std::function<Element()> render,
    std::function<void()> on_click, const Color& inactive_color)
{
    ButtonOption option;
    option.on_click  = on_click;
    option.transform = [render = std::move(render), inactive_color](
                           const EntryState& state) {
        Element element = render();
        if (state.focused) {
            return hbox(
                { std::move(element) | bold | underlined | color(PANEL_FG),
                    filler() });
        }
        return hbox({ std::move(element) | color(inactive_color), filler() });
    };
    return space_activates(Button(std::move(option)), std::move(on_click));
}

Component inline_link_button(std::string label, std::function<void()> on_click,
    const Color& inactive_color)
{
    auto shared_label = std::make_shared<const std::string>(std::move(label));
    return inline_link_button([shared_label] { return text(*shared_label); },
        std::move(on_click), inactive_color);
}

Component split_inline_link_button(std::string primary, std::string secondary,
    std::function<void()> on_click, const Color& primary_color)
{
    ButtonOption option;
    option.on_click = on_click;
    option.transform
        = [primary = std::move(primary), secondary = std::move(secondary),
              primary_color](const EntryState& state) {
              Elements parts { text(primary) | bold | color(primary_color) };
              if (!secondary.empty()) {
                  parts.push_back(text(" "));
                  parts.push_back(text(secondary)
                      | color(state.focused ? PANEL_FG : PANEL_FG_DIM));
              }
              Element element = hbox(std::move(parts));
              if (state.focused) {
                  element = std::move(element) | underlined;
              }
              return hbox({ std::move(element), filler() });
          };
    return space_activates(Button(std::move(option)), std::move(on_click));
}

std::string elapsed_text(std::chrono::milliseconds elapsed)
{
    const auto total = std::max<std::int64_t>(0, elapsed.count()) / 1000;
    return std::format("{}:{:02}", total / 60, total % 60);
}

Element card(Element body, std::optional<Color> bg, bool pad)
{
    Element inner = pad
        ? vbox({ separatorEmpty(), std::move(body), separatorEmpty() })
        : vbox({ std::move(body) });
    Element box   = vbox({ separatorEmpty(),
        hbox({ text("  "), std::move(inner) | xflex, text("  ") }),
        separatorEmpty() });
    if (bg) {
        box = std::move(box) | bgcolor(*bg) | color(PANEL_FG);
    }
    return std::move(box) | xflex;
}

Element section_title(std::string_view title, Color fg)
{
    return text(" " + std::string(title)) | bold | color(fg);
}

bool diff_row_left_changed(const DiffRow& row)
{
    return !row.left.empty() && (row.right.empty() || row.left != row.right);
}

bool diff_row_right_changed(const DiffRow& row)
{
    return !row.right.empty() && (row.left.empty() || row.left != row.right);
}

std::string diff_marker(bool added) { return added ? "+" : "−"; }

Color diff_background(bool added)
{
    return added ? DIFF_ADDITION_BG : DIFF_DELETION_BG;
}

int diff_side_width(int width) { return std::max(20, (width - 3) / 2); }

int diff_content_width(int width) { return std::max(1, width - 14); }

int review_content_width(const LayoutCtx& ctx)
{
    return ctx.kind == LayoutCtx::Kind::WIDE
        ? ctx.width - LayoutCtx::PANEL_WIDTH - 4
        : ctx.width;
}

Element diffstat_chip(std::size_t additions, std::size_t deletions)
{
    return hbox({
        text("+" + std::to_string(additions)) | color(HL_GREEN),
        text(" "),
        text("−" + std::to_string(deletions)) | color(HL_RED),
    });
}

namespace {

    // One terminal cell spans 2x4 canvas pixels.
    constexpr int CELL_X         = 2;
    constexpr int CELL_Y         = 4;
    constexpr int CHART_HEIGHT   = 64;
    constexpr int SURFACE_HEIGHT = 72;
    constexpr int AXIS_MARGIN    = 26;
    constexpr int PLOT_TOP       = 4;
    // Charts render at one fixed size; wide terminals must not stretch
    // them, so the width only shrinks below this when the chat column
    // is narrower.
    constexpr int FIXED_WIDTH_PX = 144;

    const Color& series_color(std::size_t index)
    {
        static const Color colors[]
            = { HL_BLUE, HL_GREEN, HL_YELLOW, HL_MAGENTA, HL_CYAN, HL_RED };
        return colors[index % 6];
    }

    int canvas_width(int available_width)
    {
        return std::min(FIXED_WIDTH_PX,
            std::max(40, (std::max(available_width, 20) - 2) * CELL_X));
    }

    std::string axis_label(double value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.4g", value);
        return buffer;
    }

    double category_value(const CanvasSeries& point)
    {
        return point.values.empty() ? 0.0 : point.values.front();
    }

    Element legend(const std::vector<std::pair<std::string, Color>>& items)
    {
        Elements rows;
        for (const auto& [label, tone] : items) {
            rows.push_back(
                hbox({ text("●") | color(tone), text(" " + label) }));
        }
        return vbox(std::move(rows));
    }

    void line_bounds(const CanvasView& view, double& vmin, double& vmax)
    {
        bool first = true;
        for (const CanvasSeries& series : view.series) {
            for (const double value : series.values) {
                if (first || value < vmin) {
                    vmin = value;
                }
                if (first || value > vmax) {
                    vmax = value;
                }
                first = false;
            }
        }
        if (vmin == vmax) {
            vmin -= 1;
            vmax += 1;
        }
    }

    Element render_line(const CanvasView& view, int width_px)
    {
        double vmin = 0;
        double vmax = 0;
        line_bounds(view, vmin, vmax);
        Canvas c(width_px, CHART_HEIGHT);
        const int left   = AXIS_MARGIN;
        const int right  = width_px - 2;
        const int bottom = CHART_HEIGHT - 6;
        c.DrawPointLine(left, PLOT_TOP, left, bottom, PANEL_FG_DIM);
        c.DrawPointLine(left, bottom, right, bottom, PANEL_FG_DIM);
        c.DrawText(0, PLOT_TOP, axis_label(vmax), PANEL_FG_DIM);
        c.DrawText(0, bottom - CELL_Y, axis_label(vmin), PANEL_FG_DIM);
        for (std::size_t s = 0; s < view.series.size(); ++s) {
            const std::vector<double>& values = view.series[s].values;
            const Color tone                  = series_color(s);
            const auto x                      = [&](std::size_t i) {
                const std::size_t n = values.size();
                return n == 1 ? (left + right) / 2
                              : left + int(i * (right - left) / (n - 1));
            };
            const auto y = [&](double value) {
                const double norm = (value - vmin) / (vmax - vmin);
                return bottom - int(norm * double(bottom - PLOT_TOP));
            };
            for (std::size_t i = 0; i + 1 < values.size(); ++i) {
                c.DrawPointLine(
                    x(i), y(values[i]), x(i + 1), y(values[i + 1]), tone);
            }
            if (values.size() == 1) {
                c.DrawPointCircle(x(0), y(values.front()), 2, tone);
            }
        }
        return canvas(std::move(c));
    }

    Element render_bar(const CanvasView& view, int width_px)
    {
        Canvas c(width_px, CHART_HEIGHT);
        // The value range always includes zero so bars grow from a
        // baseline at 0.
        double vmin = 0;
        double vmax = 0;
        for (const CanvasSeries& point : view.series) {
            vmin = std::min(vmin, category_value(point));
            vmax = std::max(vmax, category_value(point));
        }
        if (vmin == vmax) {
            vmax += 1;
        }
        const int left     = AXIS_MARGIN;
        const int right    = width_px - 2;
        const int top      = PLOT_TOP;
        const int bottom   = CHART_HEIGHT - 12;
        const auto block_y = [&](double value) {
            const double norm = (value - vmin) / (vmax - vmin);
            return (bottom - int(norm * double(bottom - top))) & ~1;
        };
        c.DrawBlockLine(left, top, left, bottom, PANEL_FG_DIM);
        c.DrawBlockLine(left, bottom, right, bottom, PANEL_FG_DIM);
        c.DrawText(0, top, axis_label(vmax), PANEL_FG_DIM);
        c.DrawText(0, bottom - CELL_Y, axis_label(vmin), PANEL_FG_DIM);

        const int n     = int(view.series.size());
        const int span  = right - left;
        const int slot  = std::max(1, span / std::max(1, n));
        const int gap   = std::min(4, slot / 4);
        const int x_end = left + n * slot;
        for (int i = 0; i < n; ++i) {
            const int x0     = left + i * slot + gap;
            const int x1     = std::min(x_end - gap, x0 + slot - 2 * gap);
            const int y0     = block_y(category_value(view.series[i]));
            const int y1     = block_y(0);
            const Color tone = series_color(std::size_t(i));
            for (int x = x0; x <= x1; ++x) {
                c.DrawBlockLine(x, y0, x, y1, tone);
            }
        }
        // Category labels below the axis, thinned so each drawn label
        // keeps at least four cells and truncated to its slot.
        const int fit         = std::max(1, span / (4 * CELL_X));
        const int step        = std::max(1, (n + fit - 1) / fit);
        const int label_cells = std::max(2, slot * step / CELL_X - 1);
        for (int i = 0; i < n; i += step) {
            c.DrawText((left + i * slot) & ~1, bottom + CELL_Y,
                view.series[i].label.substr(0, std::size_t(label_cells)),
                PANEL_FG_DIM);
        }
        return canvas(std::move(c));
    }

    Element render_pie(const CanvasView& view, int width_px)
    {
        double total = 0;
        for (const CanvasSeries& point : view.series) {
            total += category_value(point);
        }
        // Capped so the chart stays compact in the chat timeline.
        const int radius = std::max(8, std::min(width_px / 2, 56) - 4);
        Canvas c(width_px, radius * 2 + 8);
        const double cx = width_px / 2.0;
        const double cy = radius + 4.0;
        double angle    = -std::numbers::pi / 2;
        for (std::size_t i = 0; i < view.series.size(); ++i) {
            const double span = std::numbers::pi * 2.0
                * category_value(view.series[i]) / total;
            const Color tone = series_color(i);
            const int steps = std::max(2, int(span * 360.0 / std::numbers::pi));
            for (int s = 0; s <= steps; ++s) {
                const double a = angle + span * double(s) / double(steps);
                c.DrawPointLine(int(cx), int(cy),
                    int(cx + radius * std::cos(a)),
                    int(cy + radius * std::sin(a)), tone);
            }
            angle += span;
        }
        return canvas(std::move(c));
    }

    Element render_surface(const CanvasView& view, int width_px)
    {
        const auto& grid = view.grid;
        if (grid.size() < 2 || grid[0].size() < 2) {
            return text("");
        }
        double zmin = grid[0][0];
        double zmax = grid[0][0];
        for (const std::vector<double>& row : grid) {
            for (const double value : row) {
                zmin = std::min(zmin, value);
                zmax = std::max(zmax, value);
            }
        }
        const double zspan = zmax == zmin ? 1.0 : zmax - zmin;
        // Isometric projection: row/col fold into a diamond, z lifts the
        // wireframe; then a uniform scale fits it into the plot area.
        const double rows    = double(grid.size());
        const double columns = double(grid[0].size());
        const double lift    = 0.35 * (rows + columns);
        const auto raw_x     = [&](std::size_t i, std::size_t j) {
            return double(j) - double(i);
        };
        const auto raw_y = [&](std::size_t i, std::size_t j) {
            const double zn = (grid[i][j] - zmin) / zspan;
            return (double(i) + double(j)) * 0.5 - zn * lift;
        };
        double x0 = raw_x(0, 0);
        double x1 = x0;
        double y0 = raw_y(0, 0);
        double y1 = y0;
        for (std::size_t i = 0; i < grid.size(); ++i) {
            for (std::size_t j = 0; j < grid[i].size(); ++j) {
                x0 = std::min(x0, raw_x(i, j));
                x1 = std::max(x1, raw_x(i, j));
                y0 = std::min(y0, raw_y(i, j));
                y1 = std::max(y1, raw_y(i, j));
            }
        }
        Canvas c(width_px, SURFACE_HEIGHT);
        const int left     = 2;
        const int right    = width_px - 2;
        const int top      = 2;
        const int bottom   = SURFACE_HEIGHT - 2;
        const double scale = std::min(
            double(right - left) / (x1 - x0), double(bottom - top) / (y1 - y0));
        const auto px = [&](double rx) {
            return int(double(left + right) / 2 + (rx - (x0 + x1) / 2) * scale);
        };
        const auto py = [&](double ry) {
            return int(double(top + bottom) / 2 + (ry - (y0 + y1) / 2) * scale);
        };
        const auto draw = [&](std::size_t i, std::size_t j) {
            if (j + 1 < grid[i].size()) {
                c.DrawPointLine(px(raw_x(i, j)), py(raw_y(i, j)),
                    px(raw_x(i, j + 1)), py(raw_y(i, j + 1)), HL_CYAN);
            }
            if (i + 1 < grid.size()) {
                c.DrawPointLine(px(raw_x(i, j)), py(raw_y(i, j)),
                    px(raw_x(i + 1, j)), py(raw_y(i + 1, j)), HL_CYAN);
            }
        };
        for (std::size_t i = 0; i < grid.size(); ++i) {
            for (std::size_t j = 0; j < grid[i].size(); ++j) {
                draw(i, j);
            }
        }
        return canvas(std::move(c));
    }

} // namespace

Element canvas_chart(const CanvasView& view, int available_width)
{
    const int width_px = canvas_width(available_width);
    Elements rows;
    if (!view.title.empty()) {
        rows.push_back(text(view.title) | bold | color(PANEL_FG));
    }
    std::vector<std::pair<std::string, Color>> legend_items;
    switch (view.kind) {
    case CanvasView::Kind::LINE:
        rows.push_back(render_line(view, width_px));
        for (std::size_t s = 0; s < view.series.size(); ++s) {
            if (!view.series[s].label.empty()) {
                legend_items.emplace_back(
                    view.series[s].label, series_color(s));
            }
        }
        break;
    case CanvasView::Kind::BAR:
        rows.push_back(render_bar(view, width_px));
        break;
    case CanvasView::Kind::PIE:
        rows.push_back(render_pie(view, width_px));
        for (std::size_t s = 0; s < view.series.size(); ++s) {
            legend_items.emplace_back(view.series[s].label, series_color(s));
        }
        break;
    case CanvasView::Kind::SURFACE:
        rows.push_back(render_surface(view, width_px));
        break;
    }
    if (!legend_items.empty()) {
        rows.push_back(legend(legend_items));
    }
    return vbox(std::move(rows));
}

Element diff_split(const DiffView& diff, int available_width)
{
    const std::string syntax = syntax_type_for_path(diff.file);
    std::size_t max_line     = 1;
    for (const DiffRow& row : diff.rows) {
        if (row.left_no) {
            max_line = std::max(max_line, *row.left_no);
        }
        if (row.right_no) {
            max_line = std::max(max_line, *row.right_no);
        }
    }
    const std::size_t number_width = digit_width(max_line);
    const auto line_number = [number_width](
                                 const std::optional<std::size_t>& value) {
        const std::string number
            = value ? std::to_string(*value) : std::string();
        return text(std::string(number_width - number.size(), ' ') + number)
            | color(PANEL_FG_DIM);
    };
    const auto is_skip
        = [](const DiffRow& row) { return row.kind == DiffRow::Kind::SKIP; };

    Elements rows;
    if (available_width < 100) {
        const int content_width = std::max(
            1, available_width - static_cast<int>(2 * number_width + 6));
        const std::string blank_gutter(2 * number_width + 4, ' ');
        for (const DiffRow& row : diff.rows) {
            if (is_skip(row)) {
                rows.push_back(hbox(
                    { text("  " + row.left) | color(PANEL_FG_DIM), filler() }));
                continue;
            }
            const auto append = [&](const std::optional<std::size_t>& old_no,
                                    const std::optional<std::size_t>& new_no,
                                    std::string marker,
                                    const std::string& content,
                                    std::optional<Color> background) {
                const Elements content_rows
                    = wrapped_content_rows(content, syntax, content_width);
                for (std::size_t i = 0; i < content_rows.size(); ++i) {
                    Elements parts;
                    if (i == 0) {
                        parts.push_back(line_number(old_no));
                        parts.push_back(text(" "));
                        parts.push_back(line_number(new_no));
                        parts.push_back(text(" "));
                        parts.push_back(text(std::move(marker) + " "));
                    } else {
                        parts.push_back(text(blank_gutter));
                    }
                    parts.push_back(content_rows[i]);
                    Element line = hbox(std::move(parts));
                    if (background) {
                        line = std::move(line) | bgcolor(*background);
                    }
                    rows.push_back(std::move(line));
                }
            };
            if (diff_row_left_changed(row)) {
                append(row.left_no, std::nullopt, diff_marker(false), row.left,
                    diff_background(false));
            }
            if (diff_row_right_changed(row)) {
                append(std::nullopt, row.right_no, diff_marker(true), row.right,
                    diff_background(true));
            }
            if (!diff_row_left_changed(row) && !diff_row_right_changed(row)) {
                append(row.left_no, row.right_no, " ", row.right, std::nullopt);
            }
        }
        return panel(vbox(std::move(rows))) | xflex;
    }

    const int side_width = diff_side_width(available_width);
    const int side_content_width
        = std::max(1, side_width - static_cast<int>(number_width) - 5);
    const std::string blank_gutter(number_width + 3, ' ');
    const auto side = [&](const std::optional<std::size_t>& number,
                          std::string marker, const std::string& content,
                          std::optional<Color> background) {
        const Elements content_rows
            = wrapped_content_rows(content, syntax, side_content_width);
        Elements visual;
        for (std::size_t i = 0; i < content_rows.size(); ++i) {
            Elements parts;
            if (i == 0) {
                parts.push_back(line_number(number));
                parts.push_back(text(" "));
                parts.push_back(text(std::move(marker) + " "));
            } else {
                parts.push_back(text(blank_gutter));
            }
            parts.push_back(content_rows[i]);
            visual.push_back(hbox(std::move(parts)));
        }
        Element line = vbox(std::move(visual)) | size(WIDTH, EQUAL, side_width);
        if (background) {
            line = std::move(line) | bgcolor(*background);
        }
        return line;
    };
    for (const DiffRow& row : diff.rows) {
        if (is_skip(row)) {
            rows.push_back(hbox(
                { text("  " + row.left) | color(PANEL_FG_DIM), filler() }));
            continue;
        }
        const bool removed = diff_row_left_changed(row);
        const bool added   = diff_row_right_changed(row);
        rows.push_back(hbox({
            side(row.left_no, removed ? diff_marker(false) : " ", row.left,
                removed ? std::optional<Color>(diff_background(false))
                        : std::nullopt),
            text(" │ ") | color(PANEL_BORDER),
            side(row.right_no, added ? diff_marker(true) : " ", row.right,
                added ? std::optional<Color>(diff_background(true))
                      : std::nullopt),
        }));
    }
    return panel(vbox(std::move(rows))) | xflex;
}
} // namespace imza
