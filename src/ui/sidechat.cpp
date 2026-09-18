#include "app/flows.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/terminal.hpp>

namespace imza {

using namespace ftxui;

namespace {

    constexpr int SIDECHAT_COLUMN_WIDTH = 45;

    ChatHints sidechat_hints()
    {
        ChatHints hints;
        hints.scroll_line.clear();
        hints.phase_line.clear();
        hints.input_hint = "  Ctrl+S hide · Ctrl+R clear";
        return hints;
    }

} // namespace

class SidechatComponent : public ComponentBase {
public:
    SidechatComponent(std::shared_ptr<ApplicationState> state,
        std::function<void()> on_focus, SidechatStatus& status)
        : state_(std::move(state))
        , on_focus_(std::move(on_focus))
        , status_(status)
        , host_(Container::Vertical({ }))
    {
        Add(host_);
    }

    Element OnRender() override
    {
        _sync();
        if (pane_ == nullptr) {
            return vbox();
        }
        const auto terminal_size = Terminal::Size();
        const LayoutCtx ctx
            = layout_context(terminal_size.dimx, terminal_size.dimy);
        const bool narrow = ctx.kind == LayoutCtx::Kind::NARROW;
        column_width_ = focused_ && narrow ? ctx.width : SIDECHAT_COLUMN_WIDTH;
        layout_       = layout_context(column_width_, ctx.height);
        if (state_->sidechat == nullptr) {
            return vbox();
        }
        const Session::StatusView usage
            = state_->sidechat->session->status_view();
        Element column
            = vbox({
                  hbox({ text("Sidechat") | bold | color(PANEL_FG), filler(),
                      text(compact_number(usage.totals.total) + " tok")
                          | dim }),
                  separatorLight(),
                  separatorEmpty(),
                  host_->Render() | yflex,
              })
            | size(WIDTH, EQUAL, column_width_) | reflect(pane_box_);
        return focused_ ? column : std::move(column) | dim;
    }

    bool OnEvent(Event event) override
    {
        if (event == Event::CtrlS) {
            if (state_->sidechat_open) {
                imza::close_sidechat(*state_);
                _unfocus();
            } else {
                imza::open_sidechat(*state_);
                _set_focused(true);
            }
            return true;
        }
        if (pane_ == nullptr) {
            return false;
        }
        if (event == Event::CtrlR) {
            imza::refresh_sidechat(*state_);
            return true;
        }
        if (modal_ != nullptr && _has_modal()) {
            return modal_->OnEvent(event);
        }
        if (!event.is_mouse()) {
            if (focused_ && pane_->OnEvent(event)) {
                return true;
            }
            return false;
        }
        const Mouse& m = event.mouse();
        if (pane_box_.Contain(m.x, m.y)) {
            if (m.button == Mouse::Left && m.motion == Mouse::Pressed
                && !focused_) {
                _set_focused(true);
            }
            return pane_->OnEvent(event);
        }
        if (m.button == Mouse::Left && m.motion == Mouse::Pressed && focused_) {
            _unfocus();
        }
        return false;
    }

private:
    friend ftxui::Component make_sidechat_component(
        std::shared_ptr<ApplicationState>, std::function<void()>,
        SidechatStatus&);

    bool _has_modal() const
    {
        return state_->sidechat != nullptr
            && state_->sidechat->session->modal().index() != 0;
    }

    void _sync()
    {
        const bool open = state_->sidechat_open;
        if (open == (pane_ != nullptr)) {
            return;
        }
        if (open) {
            modal_ = make_modal(state_->sidechat);
            pane_  = make_chat(
                state_->sidechat, [this] { return layout_; }, sidechat_hints());
            status_.modal = modal_;
            host_->Add(modal_);
            host_->Add(pane_);
            return;
        }
        pane_.reset();
        modal_.reset();
        status_.modal.reset();
        host_->DetachAllChildren();
        _unfocus();
    }

    void _unfocus()
    {
        if (!focused_) {
            return;
        }
        _set_focused(false);
        on_focus_();
    }

    void _set_focused(bool focused)
    {
        focused_        = focused;
        status_.focused = focused;
    }

    std::shared_ptr<ApplicationState> state_;
    std::function<void()> on_focus_;
    SidechatStatus& status_;
    Component host_;
    Component pane_;
    Component modal_;
    bool focused_     = false;
    LayoutCtx layout_ = layout_context(0);
    int column_width_ { SIDECHAT_COLUMN_WIDTH };
    ftxui::Box pane_box_ { };
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
