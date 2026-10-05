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
    // one, the annotator pane appears beside the chat (50/50 wide, chat
    // left; alternating full-pane narrow) and Ctrl+S moves focus between
    // the two.
    class PlanTab : public ComponentBase {
    public:
        PlanTab(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            Component chat)
            : _state(std::move(state))
            , _layout(std::move(layout))
            , _chat(std::move(chat))
            , _doc(make_plan_doc(
                  _state, [this] { return _pane_ctx(); }, &_doc_focused))
        {
            Add(_doc);
        }

        Element OnRender() override
        {
            const LayoutCtx ctx = _layout();
            const bool has_plan = _has_plan();
            if (!has_plan) {
                return _chat->Render();
            }
            if (ctx.kind == LayoutCtx::Kind::NARROW) {
                return _doc_focused ? _doc->Render() : _chat->Render();
            }
            return hbox({
                _chat->Render() | size(WIDTH, EQUAL, ctx.width / 2)
                    | reflect(_chat_box),
                separatorEmpty(),
                _doc->Render() | xflex | reflect(_doc_box),
            });
        }

        bool OnEvent(Event event) override
        {
            const bool has_plan = _has_plan();
            if (!has_plan) {
                return _chat->OnEvent(event);
            }
            if (is_sidechat_toggle(event)) {
                _toggle_focus();
                return true;
            }
            if (_handle_focus_click(event)) {
                return true;
            }
            // Wheel over the doc pane scrolls it even when unfocused
            // (the dim pane stays wheel-scroll only).
            if (event.is_mouse()
                && (event.mouse().button == Mouse::WheelUp
                    || event.mouse().button == Mouse::WheelDown)
                && _doc_box.Contain(event.mouse().x, event.mouse().y)) {
                return _doc->OnEvent(event);
            }
            return _doc_focused ? _doc->OnEvent(event) : _chat->OnEvent(event);
        }

        Component ActiveChild() override
        {
            return _has_plan() && _doc_focused ? _doc : _chat;
        }

    private:
        // The doc pane occupies the right half in wide layout; it must
        // measure wrap and scroll widths against its own columns, not the
        // terminal's.
        LayoutCtx _pane_ctx() const
        {
            LayoutCtx ctx = _layout();
            if (ctx.kind == LayoutCtx::Kind::WIDE) {
                ctx.width = std::max(20, ctx.width / 2 - 1);
            }
            return ctx;
        }

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

        bool _has_plan() const { return !_state->session->plan_doc().empty(); }

        void _toggle_focus()
        {
            _doc_focused = !_doc_focused;
            if (_doc_focused) {
                _doc->TakeFocus();
            } else {
                _chat->TakeFocus();
            }
        }

        std::shared_ptr<ApplicationState> _state;
        LayoutFn _layout;
        Component _chat;
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
