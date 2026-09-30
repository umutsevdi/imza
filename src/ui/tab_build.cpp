#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

namespace imza {

namespace {

    using namespace ftxui;

    class BuildTab : public ComponentBase {
    public:
        BuildTab(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            Component chat, Component sidechat, SidechatStatus& status)
            : _state(std::move(state))
            , _layout(std::move(layout))
            , _chat(std::move(chat))
            , _sidechat(std::move(sidechat))
            , _status(status)
        {
        }

        Component chat() { return _chat; }

    private:
        std::shared_ptr<ApplicationState> _state;
        LayoutFn _layout;
        Component _chat;
        Component _sidechat;
        SidechatStatus& _status;
    };

} // namespace

Component make_build_tab(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, Component chat, Component sidechat, SidechatStatus& status)
{
    return ftxui::Make<BuildTab>(std::move(state), std::move(layout),
        std::move(chat), std::move(sidechat), status);
}

} // namespace imza
