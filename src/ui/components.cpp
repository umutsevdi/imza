#include "ui.h"
#include "util.h"

#include <algorithm>
#include <cstdlib>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
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

LayoutCtx layout_context(int width)
{
    return { width >= LayoutCtx::wide_threshold ? LayoutCtx::Kind::WIDE
                                                : LayoutCtx::Kind::NARROW,
        width };
}

using namespace ftxui;

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

Element diff_split(const DiffView& diff)
{
    const Color bg     = PANEL_COLOR;
    const Color fg     = PANEL_FG;
    const Color gutter = PANEL_FG_DIM;
    const Color border = PANEL_BORDER;
    const Color red    = Color::Red;
    const Color addbg  = Color::Green;

    const std::size_t cap = 60;
    struct Pair {
        std::string left;
        std::string right;
        bool skip = false;
    };
    std::vector<Pair> pairs;
    std::size_t max_l = 0;
    std::size_t max_r = 0;
    for (const auto& r : diff.rows) {
        Pair p;
        const bool is_skip = !r.left.empty() && r.left == r.right
            && r.left.find("unchanged line") != std::string::npos;
        if (is_skip) {
            p.skip = true;
            p.left = "  " + r.left;
        } else {
            p.left  = r.left;
            p.right = r.right;
        }
        max_l = std::max(max_l, p.left.size());
        max_r = std::max(max_r, p.right.size());
        pairs.push_back(std::move(p));
    }
    if (max_l > cap) {
        max_l = cap;
    }
    if (max_r > cap) {
        max_r = cap;
    }

    auto clip = [](std::string s, std::size_t w) {
        if (s.size() > w) {
            s = s.substr(0, w > 1 ? w - 1 : w);
            if (w > 1) {
                s += "…";
            }
        } else if (s.size() < w) {
            s.append(w - s.size(), ' ');
        }
        return s;
    };

    std::size_t max_left_no  = 1;
    std::size_t max_right_no = 1;
    for (const auto& row : diff.rows) {
        if (row.left_no) {
            max_left_no = std::max(max_left_no, *row.left_no);
        }
        if (row.right_no) {
            max_right_no = std::max(max_right_no, *row.right_no);
        }
    }
    const std::size_t left_no_width  = digit_width(max_left_no);
    const std::size_t right_no_width = digit_width(max_right_no);
    const auto line_number = [&](const std::optional<std::size_t>& no,
                                 std::size_t width) {
        const std::string value = no ? std::to_string(*no) : std::string();
        return text(std::string(width - value.size(), ' ') + value)
            | color(gutter) | dim;
    };

    Elements rows;
    for (std::size_t i = 0; i < diff.rows.size(); ++i) {
        const auto& r = diff.rows[i];
        const auto& p = pairs[i];
        if (p.skip) {
            rows.push_back(
                hbox({ text(clip(p.left, max_l + 2)) | dim | color(gutter),
                    filler() })
                | bgcolor(bg));
            continue;
        }
        const std::string ls = clip(p.left, max_l);
        const std::string rs = clip(p.right, max_r);
        const bool left_red
            = !r.left.empty() && (r.right.empty() || r.right != r.left);
        const bool right_add
            = !r.right.empty() && (r.left.empty() || r.left != r.right);
        Element L = text(ls)
            | (left_red ? bgcolor(red) | color(Color::Black) : color(fg));
        Element R = text(rs)
            | (right_add ? bgcolor(addbg) | color(Color::Black) : color(fg));
        rows.push_back(hbox({
            line_number(r.left_no, left_no_width),
            text(" "),
            L,
            text(" │ ") | color(border),
            line_number(r.right_no, right_no_width),
            text(" "),
            R,
        }));
    }
    return vbox(std::move(rows)) | bgcolor(bg) | xflex;
}
} // namespace ursa
