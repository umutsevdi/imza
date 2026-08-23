#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace ursa {

using namespace ftxui;

ftxui::Component make_side_panel(Controller& controller)
{
    constexpr int width = 30;
    return ftxui::Renderer([&controller] {
        using namespace ftxui;
        LayoutCtx ctx { LayoutCtx::Kind::WIDE, width };
        return vbox({
                   render_todo(controller.state().todo, ctx) | yflex,
                   render_changed_files(controller.state().changed_files, ctx)
                       | yflex,
               })
            | size(WIDTH, EQUAL, width);
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
            text(f.path),
        });
    }

} // namespace

Element render_todo(const TodoList& todo, const LayoutCtx&)
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
