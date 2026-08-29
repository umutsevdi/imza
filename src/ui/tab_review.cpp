#include "ui.h"
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
namespace ursa {
using namespace ftxui;

class Review : public ComponentBase {
public:
    Review(std::shared_ptr<Session> session, Controller& controller,
        LayoutFn layout)
        : session_(std::move(session))
        , controller_(controller)
        , layout_(std::move(layout))
    {
    }

    Element OnRender() override { return text("No content"); }

private:
    std::shared_ptr<Session> session_;
    Controller& controller_;
    LayoutFn layout_;
};

Component make_review(
    std::shared_ptr<Session> session, Controller& controller, LayoutFn layout)
{
    return ftxui::Make<Review>(
        std::move(session), controller, std::move(layout));
}

} // namespace ursa
