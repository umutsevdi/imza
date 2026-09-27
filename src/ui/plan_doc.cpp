#include "app/flows.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

namespace imza {

namespace {

    using namespace ftxui;

    // How long after the last keystroke the buffer writes through to the
    // session document (the agent reads the plan via imza.plan.get).
    constexpr auto WRITE_THROUGH_DELAY = std::chrono::milliseconds(800);

    // PLAN document pane: focus IS the mode. Focused, the pane is a
    // plain-text editor over the markdown source with write-through;
    // unfocused, it renders the current document as markdown (preview).
    // Revisions are agent-authored documents; user tweaks patch the
    // current document in place via edit_plan. If the agent supersedes
    // the document while the user edits, the buffer rebases to the
    // incoming revision.
    class PlanDocPane : public ComponentBase {
    public:
        PlanDocPane(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            const bool* focused)
            : _state(std::move(state))
            , _layout(std::move(layout))
            , _focused(focused)
            , _plan_subscription(
                  _state->session->subscribe_to_plan_change([this] {
                      _on_plan_changed.store(true);
                      animation::RequestAnimationFrame();
                  }))
            , _input(Input(&_buffer,
                  multiline_field_option(&_buffer, &_cursor, "", [this] {
                      _last_keystroke = std::chrono::steady_clock::now();
                      _dirty_buffer   = true;
                  })))
        {
            Add(_input);
        }

        Element OnRender() override
        {
            maybe_reload();
            write_through();
            const LayoutCtx ctx = _layout();
            Elements rows;
            rows.push_back(header());
            rows.push_back(separatorLight());
            if (!_has_plan) {
                rows.push_back(
                    paragraph("No plan yet - ask for one in the "
                              "chat, or send review comments to plan.")
                    | color(PANEL_FG_DIM) | flex);
                return vbox(std::move(rows)) | xflex | yflex;
            }
            if (*_focused) {
                rows.push_back(_input->Render() | yflex);
            } else {
                const int width = std::max(20, ctx.width - 4);
                rows.push_back(
                    scroll_viewport(
                        render_markdown_element(_current, width), _viewport)
                    | yflex);
            }
            return vbox(std::move(rows)) | xflex | yflex;
        }

        bool OnEvent(Event event) override
        {
            if (!_has_plan) {
                return false;
            }
            if (*_focused) {
                if (event == Event::Return) {
                    // Multiline input: newline, never a submit.
                    return _input->OnEvent(event);
                }
                return _input->OnEvent(event);
            }
            if (scroll_viewport_event(_viewport, event)) {
                return true;
            }
            return false;
        }

        // Reload from the session when a plan change was signaled.
        void maybe_reload()
        {
            // The signal flag plus a direct comparison: missed signals
            // (pane constructed after a plan existed, restored sessions)
            // must still converge on the session state.
            const bool signaled           = _on_plan_changed.exchange(false);
            const std::string session_doc = _state->session->plan_doc();
            if (!signaled && session_doc == _current) {
                return;
            }
            const SessionSnapshot snap = _state->session->snapshot();
            _has_plan                  = !snap.plans.empty();
            _revision                  = snap.plans.size();
            const std::string fresh
                = _has_plan ? snap.plans.back().content : std::string();
            if (*_focused && _dirty_buffer && fresh != _last_seen) {
                // The agent superseded the document while the user edits:
                // the requested rewrite wins over mid-flight keystrokes.
                // Rebase for real: the stale buffer must never reach the
                // session via write_through.
                _buffer       = fresh;
                _dirty_buffer = false;
                _status = "agent updated the plan - your draft was replaced";
            }
            _current   = fresh;
            _last_seen = fresh;
            if (!_dirty_buffer) {
                _buffer = fresh;
            }
        }

        // Applies pending buffer edits to the session document.
        void write_through()
        {
            if (!_dirty_buffer) {
                return;
            }
            // Blurring the editor writes immediately; while focused the
            // write is debounced so the buffer is not shipped per
            // keystroke.
            if (*_focused
                && std::chrono::steady_clock::now() - _last_keystroke
                    < WRITE_THROUGH_DELAY) {
                return;
            }
            if (_buffer == _last_seen) {
                _dirty_buffer = false;
                return;
            }
            {
                // Never write over a document that changed since we last
                // saw it: reload the buffer from the session instead.
                const SessionSnapshot snap     = _state->session->snapshot();
                const std::string& session_doc = snap.plans.empty()
                    ? _last_seen
                    : snap.plans.back().content;
                if (session_doc != _last_seen) {
                    _current   = session_doc;
                    _last_seen = session_doc;
                    _buffer    = session_doc;
                    _status
                        = "agent updated the plan - your draft was replaced";
                    _dirty_buffer = false;
                    return;
                }
            }
            const std::string error
                = _state->session->edit_plan(_last_seen, _buffer, 0);
            if (error.empty()) {
                _last_seen    = _buffer;
                _current      = _buffer;
                _dirty_buffer = false;
            } else {
                // The document moved underneath (agent supersede race):
                // rebase the editor to the incoming revision.
                _status    = "agent updated the plan - your draft was replaced";
                _buffer    = _current;
                _last_seen = _current;
                _dirty_buffer = false;
            }
        }

    private:
        Element header()
        {
            const std::string label
                = _has_plan ? plan_revision_label(_revision - 1) : "No plan";
            Elements parts {
                text("Plan") | bold | color(PANEL_FG),
                text(" · " + label) | color(PANEL_FG_DIM),
            };
            if (!_status.empty()) {
                parts.push_back(text(" · " + _status) | color(HL_YELLOW));
            }
            return hbox(std::move(parts)) | xflex;
        }

        std::shared_ptr<ApplicationState> _state;
        LayoutFn _layout;
        const bool* _focused;
        Signal<>::Subscription _plan_subscription;
        Component _input;
        std::string _buffer;
        int _cursor           = 0;
        bool _has_plan        = false;
        std::size_t _revision = 0;
        std::string _current;
        std::string _last_seen;
        std::string _status;
        bool _dirty_buffer = false;
        std::chrono::steady_clock::time_point _last_keystroke {
            std::chrono::steady_clock::now()
        };
        std::atomic<bool> _on_plan_changed { false };
        ScrollView _viewport { };
    };

} // namespace

Component make_plan_doc(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, const bool* focused)
{
    return ftxui::Make<PlanDocPane>(
        std::move(state), std::move(layout), focused);
}

} // namespace imza
