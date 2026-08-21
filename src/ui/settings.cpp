#include "ui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace ursa {

ftxui::Component make_settings(Controller&)
{
    return ftxui::Renderer([] {
        using namespace ftxui;
        return dim(text("[settings modal]"));
    });
}

} // namespace ursa
