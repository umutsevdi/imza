#include "render.h"

#include <ftxui/dom/elements.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "format.h"

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
            box = std::move(box) | bgcolor(*bg) | color(PANEL_FG);
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
        return card(render_markdown_element(tool_call_markdown(tc)));
    }

    Element modal_answer_item(const ModalAnswer& ans)
    {
        return card(
            render_markdown_element(modal_answer_markdown(ans)), PANEL_COLOR);
    }

    Element section_title(std::string_view title)
    {
        return text(std::string(title)) | bold | color(PANEL_FG_DIM);
    }

} // namespace

std::string tool_call_markdown(const ToolCall& call)
{
    std::string md
        = tool_request_markdown(ToolCallRequest { call.name, call.args, "" });
    if (!call.result.has_value()) {
        return md;
    }
    switch (call.result->kind) {
    case ToolCall::Result::Kind::OUTPUT:
        md += "\n\n---\n\n" + call.result->text;
        break;
    case ToolCall::Result::Kind::REJECT:
        md += "\n\n---\n\nUser answered:\n> Rejected:";
        if (!call.result->text.empty()) {
            md += " " + call.result->text;
        }
        break;
    case ToolCall::Result::Kind::CANCEL: break;
    }
    return md;
}

Element render_todo(const TodoList& todo, const LayoutCtx& ctx [[maybe_unused]])
{
    Elements parts;
    for (const auto& it : todo.items) {
        const std::string mark = it.done ? "[x]" : "[ ]";
        Element line = hbox({ dim(text(mark)), text(" "), text(it.text) });
        parts.push_back(std::move(line));
    }
    Element body = parts.empty()
        ? dim(text("none"))
        : vbox(std::move(parts)) | borderStyled(ROUNDED, PANEL_BORDER);
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
            } else if constexpr (std::is_same_v<T, ModalAnswer>) {
                return modal_answer_item(v);
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
    Element body = parts.empty()
        ? dim(text("no changes"))
        : vbox(std::move(parts)) | borderStyled(ROUNDED, PANEL_BORDER);
    return vbox({ section_title("Changed files"), std::move(body) });
}

Element render_help(const std::vector<SlashCommand>& commands)
{
    Elements rows;
    for (const auto& c : commands) {
        rows.push_back(hbox({
            text(c.name) | bold | color(PANEL_FG),
            text("   "),
            text(c.desc) | dim | color(PANEL_FG_DIM),
        }));
    }
    return vbox({
        section_title("Commands"),
        vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER),
    });
}

} // namespace ursa
