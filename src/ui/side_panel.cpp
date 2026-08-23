#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "render.h"

namespace ursa {

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

} // namespace ursa
