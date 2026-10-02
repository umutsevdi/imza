#include "app/flows.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/terminal.hpp>

namespace imza {

using namespace ftxui;

namespace {

    ChatHints sidechat_hints()
    {
        ChatHints hints;
        hints.scroll_line.clear();
        hints.phase_line.clear();
        hints.input_hint         = "  Ctrl+S hide · Ctrl+R clear";
        hints.empty_state_banner = false;
        return hints;
    }

} // namespace

class SidechatComponent : public ComponentBase {
public:
    SidechatComponent(std::shared_ptr<ApplicationState> state,
        std::function<void()> on_focus, SidechatStatus& status)
        : _state(std::move(state))
        , _on_focus(std::move(on_focus))
        , _status(status)
        , _host(Container::Vertical({ }))
    {
        Add(_host);
    }

    Element OnRender() override
    {
        _sync();
        if (_pane == nullptr) {
            return vbox();
        }
        const auto terminal_size = Terminal::Size();
        const LayoutCtx ctx
            = layout_context(terminal_size.dimx, terminal_size.dimy);
        const bool narrow = ctx.kind == LayoutCtx::Kind::NARROW;
        const int column_width
            = _focused && narrow ? ctx.width : LayoutCtx::RIGHT_WIDTH;
        _layout = layout_context(column_width, ctx.height);
        if (_state->sidechat == nullptr) {
            return vbox();
        }
        const Session::StatusView usage
            = _state->sidechat->session->status_view();
        Element column
            = vbox({
                  hbox({ text("Sidechat") | bold | color(PANEL_FG), filler(),
                      text(compact_number(usage.totals.total) + " tok")
                          | dim }),
                  separatorLight(),
                  separatorEmpty(),
                  _host->Render() | yflex,
              })
            | yflex | size(WIDTH, EQUAL, column_width) | reflect(_pane_box);
        return _focused ? column : std::move(column) | dim;
    }

    bool OnEvent(Event event) override
    {
        if (event == Event::CtrlS) {
            if (_state->sidechat_open) {
                imza::close_sidechat(*_state);
                _unfocus();
            } else {
                imza::open_sidechat(*_state);
                _set_focused(true);
            }
            return true;
        }
        if (_pane == nullptr) {
            return false;
        }
        if (event == Event::CtrlR) {
            imza::refresh_sidechat(*_state);
            return true;
        }
        if (_modal != nullptr && _has_modal()) {
            return _modal->OnEvent(event);
        }
        if (!event.is_mouse()) {
            if (_focused && _pane->OnEvent(event)) {
                return true;
            }
            return false;
        }
        const Mouse& m = event.mouse();
        if (_pane_box.Contain(m.x, m.y)) {
            if (m.button == Mouse::Left && m.motion == Mouse::Pressed
                && !_focused) {
                _set_focused(true);
            }
            return _pane->OnEvent(event);
        }
        if (m.button == Mouse::Left && m.motion == Mouse::Pressed && _focused) {
            _unfocus();
        }
        return false;
    }

    void OnAnimation(animation::Params& params) override
    {
        _host->OnAnimation(params);
    }

private:
    friend ftxui::Component make_sidechat_component(
        std::shared_ptr<ApplicationState>, std::function<void()>,
        SidechatStatus&);

    bool _has_modal() const
    {
        return _state->sidechat != nullptr
            && _state->sidechat->session->modal().index() != 0;
    }

    void _sync()
    {
        const bool open = _state->sidechat_open;
        if (open == (_pane != nullptr)) {
            return;
        }
        if (open) {
            _modal = make_modal(_state->sidechat);
            _pane  = make_chat(
                _state->sidechat, [this] { return _layout; }, sidechat_hints());
            _status.modal = _modal;
            _host->Add(_modal);
            _host->Add(_pane);
            return;
        }
        _pane.reset();
        _modal.reset();
        _status.modal.reset();
        _host->DetachAllChildren();
        _unfocus();
    }

    void _unfocus()
    {
        if (!_focused) {
            return;
        }
        _set_focused(false);
        _on_focus();
    }

    void _set_focused(bool focused)
    {
        _focused        = focused;
        _status.focused = focused;
    }

    std::shared_ptr<ApplicationState> _state;
    std::function<void()> _on_focus;
    SidechatStatus& _status;
    Component _host;
    Component _pane;
    Component _modal;
    bool _focused     = false;
    LayoutCtx _layout = layout_context(0);
    ftxui::Box _pane_box { };
};

ftxui::Component make_sidechat_component(
    std::shared_ptr<ApplicationState> state, std::function<void()> on_focus,
    SidechatStatus& status)
{
    auto component = ftxui::Make<SidechatComponent>(
        std::move(state), std::move(on_focus), status);
    std::weak_ptr<ComponentBase> weak = component;
    status.has_modal                  = [weak, &status] {
        const auto locked = weak.lock();
        return locked != nullptr
            && std::static_pointer_cast<SidechatComponent>(locked)->_has_modal()
            && status.modal != nullptr;
    };
    return component;
}

} // namespace imza
