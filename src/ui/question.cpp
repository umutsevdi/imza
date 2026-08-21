#include "ui.h"

#include <ftxui/component/component.hpp>

#include "render.h"
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
