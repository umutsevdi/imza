#include "ui.hpp"

#include <ftxui/component/component.hpp>

#include "render.hpp"
#include <ftxui/dom/elements.hpp>

#include <functional>

namespace ursa {

ftxui::Component make_todo(Controller& controller, std::function<int()> width)
{
    return ftxui::Renderer([&controller, width] {
        using namespace ftxui;
        LayoutCtx ctx { LayoutCtx::Kind::WIDE, width() };
        return render_todo(controller.state().todo, ctx);
    });
}

} // namespace ursa
