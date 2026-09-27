#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

namespace imza {

namespace {

    using namespace ftxui;

    class PlanTab : public ComponentBase {
    public:
        PlanTab(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            Component chat)
            : state_(std::move(state))
            , layout_(std::move(layout))
            , chat_(std::move(chat))
        {
        }

        Component chat() { return chat_; }

    private:
        std::shared_ptr<ApplicationState> state_;
        LayoutFn layout_;
        Component chat_;
    };

} // namespace

Component make_plan_tab(
    std::shared_ptr<ApplicationState> state, LayoutFn layout, Component chat)
{
    return ftxui::Make<PlanTab>(
        std::move(state), std::move(layout), std::move(chat));
}

} // namespace imza
