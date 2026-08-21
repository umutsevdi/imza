#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace ursa {

ftxui::Component make_toolcall(Controller&)
{
    return ftxui::Renderer([] {
        using namespace ftxui;
        return dim(text("[tool call]"));
    });
}

} // namespace ursa
