#include "ui.hpp"

#include <ftxui/component/component.hpp>

#include "render.hpp"
#include <ftxui/dom/elements.hpp>

namespace ursa {

ftxui::Component make_question(Controller& controller)
{
    return ftxui::Renderer([&controller] {
        using namespace ftxui;
        if (const auto* q = controller.state().question
                ? &*controller.state().question
                : nullptr) {
            return render_question(*q) | borderLight;
        }
        return text("");
    });
}

} // namespace ursa
