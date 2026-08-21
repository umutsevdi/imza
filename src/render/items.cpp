#include "render.h"

#include <ftxui/dom/elements.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    Element card(Element body, std::optional<Color> bg = std::nullopt)
    {
        Element inner
            = vbox({ separatorEmpty(), std::move(body), separatorEmpty() });
        Element box
            = hbox({ text("  "), std::move(inner) | xflex, text("  ") });
        if (bg) {
            box = std::move(box) | bgcolor(*bg);
        }
        return std::move(box) | xflex;
    }

    Element user_item(const UserTurn& t)
    {
        return card(render_markdown_element(t.text), PANEL_COLOR);
    }

    Element assistant_item(const AssistantTurn& t)
    {
        return card(render_markdown_element(t.markdown));
    }

    Element toolcall_item(const ToolCall& tc)
    {
        return vbox({
            hbox({
                text("tool › ") | color(Color::YellowLight) | bold,
                text(tc.name),
            }),
            paragraph(tc.args),
        });
    }

    Element section_title(std::string_view title)
    {
        return text(std::string(title)) | bold | color(Color::GrayLight);
    }

    Element question_item(const Question& q)
    {
        Elements opts;
        for (const auto& o : q.options) {
            opts.push_back(hbox({ text("• "), text(o) }));
        }
        return vbox({
            hbox({
                text("? ") | color(Color::MagentaLight) | bold,
                text(q.prompt),
            }),
            vbox(std::move(opts)),
        });
    }

} // namespace

Element render_todo(const TodoList& todo, const LayoutCtx& ctx [[maybe_unused]])
{
    Elements parts;
    for (const auto& it : todo.items) {
        const std::string mark = it.done ? "[x]" : "[ ]";
        Element line = hbox({ dim(text(mark)), text(" "), text(it.text) });
        parts.push_back(std::move(line));
    }
    Element body = parts.empty() ? dim(text("none"))
                                 : vbox(std::move(parts)) | borderRounded;
    return vbox({ section_title("Todo"), std::move(body) });
}

Element render_item(const ConversationItem& item, const LayoutCtx& ctx)
{
    return std::visit(
        [&](const auto& v) -> Element {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, UserTurn>) {
                return user_item(v);
            } else if constexpr (std::is_same_v<T, AssistantTurn>) {
                return assistant_item(v);
            } else if constexpr (std::is_same_v<T, ToolCall>) {
                return toolcall_item(v);
            } else if constexpr (std::is_same_v<T, TodoList>) {
                return render_todo(v, ctx);
            } else if constexpr (std::is_same_v<T, Question>) {
                return question_item(v);
            }
            return text("");
        },
        item);
}

namespace {

    Element changed_file_item(const ChangedFile& f)
    {
        Color c = Color::GrayDark;
        if (f.status == "M") {
            c = Color::YellowLight;
        } else if (f.status == "A") {
            c = Color::GreenLight;
        } else if (f.status == "D") {
            c = Color::RedLight;
        }
        return hbox({
            text(f.status) | color(c) | bold,
            text(" "),
            text(f.path),
        });
    }

} // namespace

Element render_changed_files(
    const std::vector<ChangedFile>& files, const LayoutCtx&)
{
    Elements parts;
    for (const auto& f : files) {
        parts.push_back(changed_file_item(f));
    }
    Element body = parts.empty() ? dim(text("no changes"))
                                 : vbox(std::move(parts)) | borderRounded;
    return vbox({ section_title("Changed files"), std::move(body) });
}

Element render_question(const Question& q)
{
    return vbox({ section_title("Question"), question_item(q) });
}

Element render_help(const std::vector<SlashCommand>& commands)
{
    Elements rows;
    for (const auto& c : commands) {
        rows.push_back(hbox({
            text(c.name) | bold | color(Color::White),
            text("   "),
            text(c.desc) | dim | color(Color::GrayLight),
        }));
    }
    return vbox({
        section_title("Commands"),
        vbox(std::move(rows)) | borderRounded,
    });
}

} // namespace ursa
