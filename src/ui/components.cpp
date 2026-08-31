#include "ui.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ursa {

std::string fit(const std::string& value, int width)
{
    const std::size_t max = static_cast<std::size_t>(std::max(width, 0));
    std::string out;
    std::size_t seen = 0;
    std::size_t i    = 0;
    while (i < value.size() && seen < max) {
        const auto lead = static_cast<unsigned char>(value[i]);
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        length = std::min(length, value.size() - i);
        if (seen + 1 == max && i + length < value.size()) return out + "…";
        out.append(value, i, length);
        ++seen;
        i += length;
    }
    out.append(max - seen, ' ');
    return out;
}

std::string fit(const std::string& value, int width, int offset)
{
    const std::size_t skip
        = static_cast<std::size_t>(std::max(offset, 0));
    std::size_t seen = 0;
    std::size_t pos  = 0;
    while (pos < value.size() && seen < skip) {
        const auto lead = static_cast<unsigned char>(value[pos]);
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        pos += std::min(length, value.size() - pos);
        ++seen;
    }
    return fit(value.substr(pos), width);
}

LayoutCtx layout_context(int width)
{
    return { width >= LayoutCtx::wide_threshold ? LayoutCtx::Kind::WIDE
                                                : LayoutCtx::Kind::NARROW,
        width };
}

using namespace ftxui;

namespace {

    std::string error_sentence(std::string message)
    {
        while (!message.empty()
            && std::isspace(static_cast<unsigned char>(message.back()))) {
            message.pop_back();
        }
        if (message.empty()) {
            return message;
        }
        message.front() = static_cast<char>(
            std::toupper(static_cast<unsigned char>(message.front())));
        if (!message.ends_with('.') && !message.ends_with('!')
            && !message.ends_with('?') && !message.ends_with("…")) {
            message += '.';
        }
        return message;
    }

}

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
        message = "Rate limited — retrying in " + std::to_string(remaining)
            + "s…";
    }
    if (message.empty()) {
        return text("");
    }
    return hbox({
        text(" ") | bgcolor(Color::Red),
        text(" " + message) | bgcolor(Color::Red),
        filler() | bgcolor(Color::Red),
    });
}

std::size_t digit_width(std::size_t n) { return std::to_string(n).size(); }

Element code_block(const std::string& code, const std::string& lang)
{
    const Color bg = PANEL_COLOR;
    const Color fg = PANEL_FG;
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
    return hbox({ text(" "), std::move(inner) | xflex, text(" ") })
        | bgcolor(bg) | dim;
}

Element code_block_with_lines(
    const std::string& code, const std::string& lang, std::size_t start_line)
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
    const std::size_t width
        = digit_width(last_num < start_line ? start_line : last_num);

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
        const std::string num = std::to_string(start_line + i);
        std::string padded(width - num.size(), ' ');
        padded += num;
        body.push_back(hbox({
            text(padded) | color(gutter),
            text(" "),
            text(lines[i]) | color(fg),
        }));
    }
    Element inner = vbox(std::move(body));
    return hbox({ text(" "), std::move(inner) | xflex, text(" ") })
        | bgcolor(bg) | dim;
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
    return text(std::string(title)) | bold | color(fg);
}

Element diff_split(const DiffView& diff, int available_width)
{
    std::size_t max_line = 1;
    for (const DiffRow& row : diff.rows) {
        if (row.left_no) max_line = std::max(max_line, *row.left_no);
        if (row.right_no) max_line = std::max(max_line, *row.right_no);
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
    const auto left_changed = [](const DiffRow& row) {
        return !row.left.empty()
            && (row.right.empty() || row.left != row.right);
    };
    const auto right_changed = [](const DiffRow& row) {
        return !row.right.empty()
            && (row.left.empty() || row.left != row.right);
    };

    Elements rows;
    if (available_width < 100) {
        for (const DiffRow& row : diff.rows) {
            if (is_skip(row)) {
                rows.push_back(hbox({ text("  " + row.left)
                                              | color(PANEL_FG_DIM),
                    filler() }));
                continue;
            }
            const auto append = [&](const std::optional<std::size_t>& old_no,
                                    const std::optional<std::size_t>& new_no,
                                    std::string marker,
                                    const std::string& content,
                                    std::optional<Color> background) {
                Element line = hbox({ line_number(old_no), text(" "),
                    line_number(new_no), text(" "),
                    text(std::move(marker) + " " + content)
                        | color(PANEL_FG),
                    filler() });
                if (background) {
                    line = std::move(line) | bgcolor(*background);
                }
                rows.push_back(std::move(line));
            };
            if (left_changed(row)) {
                append(row.left_no, std::nullopt, "−", row.left,
                    DIFF_DELETION_BG);
            }
            if (right_changed(row)) {
                append(std::nullopt, row.right_no, "+", row.right,
                    DIFF_ADDITION_BG);
            }
            if (!left_changed(row) && !right_changed(row)) {
                append(row.left_no, row.right_no, " ", row.right,
                    std::nullopt);
            }
        }
        return panel(vbox(std::move(rows))) | xflex;
    }

    const int side_width = std::max(20, (available_width - 3) / 2);
    const auto side = [&](const std::optional<std::size_t>& number,
                          std::string marker, const std::string& content,
                          std::optional<Color> background) {
        Element line = hbox({ line_number(number), text(" "),
            text(std::move(marker) + " " + content) | color(PANEL_FG),
            filler() }) | size(WIDTH, EQUAL, side_width);
        if (background) line = std::move(line) | bgcolor(*background);
        return line;
    };
    for (const DiffRow& row : diff.rows) {
        if (is_skip(row)) {
            rows.push_back(hbox({ text("  " + row.left)
                                          | color(PANEL_FG_DIM),
                filler() }));
            continue;
        }
        const bool removed = left_changed(row);
        const bool added = right_changed(row);
        rows.push_back(hbox({
            side(row.left_no, removed ? "−" : " ", row.left,
                removed ? std::optional<Color>(DIFF_DELETION_BG)
                        : std::nullopt),
            text(" │ ") | color(PANEL_BORDER),
            side(row.right_no, added ? "+" : " ", row.right,
                added ? std::optional<Color>(DIFF_ADDITION_BG)
                      : std::nullopt),
        }));
    }
    return panel(vbox(std::move(rows))) | xflex;
}
} // namespace ursa
