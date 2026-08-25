#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace ursa {

using namespace ftxui;

ftxui::Component make_side_panel(Controller& controller,
    std::function<int()> width)
    {
        return ftxui::Renderer(
            [&controller, width = std::move(width)] {
                using namespace ftxui;
                const int w = width();
                const bool narrow = w < LayoutCtx::wide_threshold;
                LayoutCtx ctx { narrow ? LayoutCtx::Kind::NARROW
                                       : LayoutCtx::Kind::WIDE,
                    w };
                Elements parts;
                parts.push_back(
                    render_todo(controller.state().todo, ctx) | yflex);
                if (!narrow) {
                    parts.push_back(
                        render_changed_files(
                            controller.state().changed_files, ctx)
                        | yflex);
                }
                const std::optional<std::string>& rules
                    = controller.state().agent_rules;
                if (!narrow && rules) {
                    parts.push_back(text(" " + *rules + " ") | bold
                        | color(Color::Black) | bgcolor(Color::Yellow)
                        | xflex);
                }
                Element body = vbox(std::move(parts));
                if (narrow) {
                    return body | xflex;
                }
                return body | size(WIDTH, EQUAL, LayoutCtx::panel_width);
            });
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
            paragraph(f.path) | xflex,
        });
    }

} // namespace

namespace {

    Element todo_item(const TodoItem& it)
    {
        using Status = TodoItem::Status;
        ftxui::Color mark_color = PANEL_FG_DIM;
        bool mark_bold          = false;
        std::string mark;
        switch (it.status) {
        case Status::IN_PROGRESS:
            mark       = "→";
            mark_color = PANEL_FG;
            mark_bold  = true;
            break;
        case Status::COMPLETED: mark = "[x]"; break;
        case Status::CANCELLED: mark = "[-]"; break;
        case Status::PENDING: mark = "[ ]"; break;
        }
        const bool inactive = it.status == Status::COMPLETED
            || it.status == Status::CANCELLED;
        Element mark_el = text(mark) | color(mark_color);
        if (mark_bold) {
            mark_el = std::move(mark_el) | bold;
        }
        Element content
            = inactive ? dim(paragraph(it.content)) : paragraph(it.content);
        return hbox(
            { std::move(mark_el), text(" "), std::move(content) | xflex });
    }

} // namespace

Element render_todo(const TodoList& todo, const LayoutCtx&)
{
    Elements parts;
    for (const auto& it : todo.items) {
        parts.push_back(todo_item(it));
    }
    Element body = parts.empty()
        ? dim(text("none"))
        : vbox(std::move(parts)) | borderStyled(ROUNDED, PANEL_BORDER);
    return vbox({ section_title("Todo"), std::move(body) });
}

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

} // namespace ursa
