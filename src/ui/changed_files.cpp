#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "render.h"

namespace ursa {

ftxui::Component make_changed_files(Controller& controller)
{
    return ftxui::Renderer([&controller] {
        using namespace ftxui;
        LayoutCtx ctx { LayoutCtx::Kind::WIDE, 30 };
        return render_changed_files(controller.state().changed_files, ctx);
    });
}

} // namespace ursa
