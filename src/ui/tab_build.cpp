#include "ui/ui.h"

#include <ftxui/component/component.hpp>

namespace imza {

// The Build tab currently hosts only the shared chat: callers attach the
// chat themselves (App::_attach_chat, the tab surface tests), so the tab
// is an empty container that renders whatever is added to it.
ftxui::Component make_build_tab(std::shared_ptr<ApplicationState>, LayoutFn,
    ftxui::Component, ftxui::Component, SidechatStatus&)
{
    return ftxui::Container::Vertical({ });
}

} // namespace imza
