#include "common/util.h"
#include "ui/ui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <functional>
#include <iterator>
#include <memory>
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
    return row;
}

ftxui::Element model_picker_row(const ModelRow& row, bool selected)
{
    ftxui::Element e = ftxui::hbox({
        ftxui::text(selected ? "› " : "  "),
        ftxui::text(row.name),
        ftxui::filler(),
        ftxui::text(row.tag) | ftxui::dim,
    });
    if (selected) {
        e = std::move(e) | ftxui::bold;
    }
    return e;
}

void ModelPickList::refill_visible()
{
    const std::string needle = to_lower(trim(filter));
    visible.clear();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!needle.empty()
            && to_lower(rows[i].model_id).find(needle) == std::string::npos
            && to_lower(rows[i].name).find(needle) == std::string::npos) {
            continue;
        }
        visible.push_back(i);
    }
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
    for (const std::string& line : split_lines(body)) {
        for (const auto& [begin, end] : wrap_row_ranges(line, width)) {
            out.emplace_back(line.substr(begin, end - begin));
        }
    }
    if (out.empty()) {
        out.emplace_back();
    }
    return out;
}

LayoutCtx layout_context(int width, int height)
{
    return { width >= LayoutCtx::wide_threshold ? LayoutCtx::Kind::WIDE
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
        message
            = "Rate limited — retrying in " + std::to_string(remaining) + "s…";
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
        ? ctx.width - LayoutCtx::panel_width - 4
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
    const auto is_skip = [](const DiffRow& row) {
        return !row.left.empty() && row.left == row.right
            && row.left.find("unchanged line") != std::string::npos;
    };

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
