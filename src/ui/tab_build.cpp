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
            : state_(std::move(state))
            , layout_(std::move(layout))
            , chat_(std::move(chat))
            , sidechat_(std::move(sidechat))
            , status_(status)
        {
        }

        Component chat() { return chat_; }

    private:
        std::shared_ptr<ApplicationState> state_;
        LayoutFn layout_;
        Component chat_;
        Component sidechat_;
        SidechatStatus& status_;
    };

} // namespace

Component make_build_tab(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, Component chat, Component sidechat, SidechatStatus& status)
{
    return ftxui::Make<BuildTab>(std::move(state), std::move(layout),
        std::move(chat), std::move(sidechat), status);
}

} // namespace imza
