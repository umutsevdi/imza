#include "app/application_state.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

namespace imza {

namespace {

    using namespace ftxui;

    // PLAN tab content: chat until a plan exists; once the agent creates
    // one, the document pane appears (50/50 wide, alternating narrow) and
    // Ctrl+S moves focus between the two. Focus decides editor vs
    // markdown preview.
    class PlanTab : public ComponentBase {
    public:
        PlanTab(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            Component chat)
            : state_(std::move(state))
            , layout_(std::move(layout))
            , chat_(std::move(chat))
            , _doc(make_plan_doc(state_, layout_, &_doc_focused))
        {
            Add(_doc);
        }

        Component chat() { return chat_; }

        Element OnRender() override
        {
            const LayoutCtx ctx = layout_();
            const bool has_plan = _has_plan();
            if (!has_plan) {
                return chat_->Render();
            }
            if (ctx.kind == LayoutCtx::Kind::NARROW) {
                return _doc_focused ? _doc->Render() : chat_->Render();
            }
            return hbox({
                _doc->Render() | xflex | reflect(_doc_box),
                separatorEmpty(),
                chat_->Render() | reflect(_chat_box)
                    | size(WIDTH, EQUAL, LayoutCtx::RIGHT_WIDTH),
            });
        }

        bool OnEvent(Event event) override
        {
            const bool has_plan = _has_plan();
            if (!has_plan) {
                return chat_->OnEvent(event);
            }
            if (is_sidechat_toggle(event)) {
                _toggle_focus();
                return true;
            }
            if (_handle_focus_click(event)) {
                return true;
            }
            return _doc_focused ? _doc->OnEvent(event) : chat_->OnEvent(event);
        }

        Component ActiveChild() override
        {
            return _has_plan() && _doc_focused ? _doc : chat_;
        }

    private:
        // A left press over a pane moves focus there; presses on the
        // separator or outside both boxes change nothing.
        bool _handle_focus_click(Event event)
        {
            if (!event.is_mouse()) {
                return false;
            }
            const Mouse& m = event.mouse();
            if (m.button != Mouse::Left || m.motion != Mouse::Pressed) {
                return false;
            }
            if (_doc_box.Contain(m.x, m.y) && !_doc_focused) {
                _toggle_focus();
                return true;
            }
            if (_chat_box.Contain(m.x, m.y) && _doc_focused) {
                _toggle_focus();
                return true;
            }
            return false;
        }

        bool _has_plan() const { return !state_->session->plan_doc().empty(); }

        void _toggle_focus()
        {
            _doc_focused = !_doc_focused;
            if (_doc_focused) {
                _doc->TakeFocus();
            } else {
                chat_->TakeFocus();
            }
        }

        std::shared_ptr<ApplicationState> state_;
        LayoutFn layout_;
        Component chat_;
        Component _doc;
        Box _doc_box { };
        Box _chat_box { };
        bool _doc_focused = false;
    };

} // namespace

Component make_plan_tab(
    std::shared_ptr<ApplicationState> state, LayoutFn layout, Component chat)
{
    return ftxui::Make<PlanTab>(
        std::move(state), std::move(layout), std::move(chat));
}

} // namespace imza
