#include "app/flows.h"
#include "turn/prompt.h"
#include "ui/annotations.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace imza {

namespace {

    using namespace ftxui;

    // PLAN document pane: block rows with Review-style note cards. Focused,
    // the pane owns the keyboard (navigation, notes, revise); unfocused it
    // renders dim, handles only wheel scroll, and lets every other key fall
    // through to the chat. The document is mutated only by the agent during
    // the revise turn, so any plan change just reloads the rows.
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
            , _editor([this] { animation::RequestAnimationFrame(); })
        {
            Add(_editor.input());
            _revise_button
                = action_button("Revise Plan", [this] { _revise(); });
            // Parented like Review's action buttons: unparented FTXUI
            // buttons report Focused() permanently and never change style.
            Add(_revise_button);
        }

        Element OnRender() override
        {
            _maybe_reload();
            const LayoutCtx ctx = _layout();
            const int width     = std::max(20, ctx.width - 4);
            _height             = ctx.height;
            Elements rows;
            rows.push_back(_header());
            rows.push_back(separatorLight());
            if (!_has_plan) {
                rows.push_back(
                    paragraph("No plan yet - ask for one in the "
                              "chat, or send review comments to plan.")
                    | color(PANEL_FG_DIM) | flex);
                return vbox(std::move(rows)) | xflex | yflex;
            }
            _build_rows(rows, width);
            if (_pending_note_select != -1) {
                for (std::size_t i = 0; i < _rows.size(); ++i) {
                    if (_rows[i].kind == Row::Kind::NOTE
                        && static_cast<int>(_rows[i].note)
                            == _pending_note_select) {
                        _selected = static_cast<int>(i);
                        break;
                    }
                }
                _pending_note_select = -1;
            }
            _selected = std::clamp(
                _selected, 0, std::max(0, static_cast<int>(_rows.size()) - 1));
            const int focus_y = _rows.empty()
                ? 0
                : _rows[static_cast<std::size_t>(_selected)].display_y;
            Element content = vbox(std::move(rows)) | focusPosition(0, focus_y)
                | yframe | vscroll_indicator | flex;
            Elements bottom;
            bottom.push_back(
                hbox({ _revise_button->Render() | reflect(_button_box),
                    filler(), text(_hint_line()) | color(PANEL_FG_DIM) }));
            if (_editor.is_open()) {
                bottom.push_back(
                    text("Enter save · Alt+Enter new line · Esc cancel")
                    | color(PANEL_FG_DIM));
            }
            Element pane = vbox({ std::move(content), vbox(std::move(bottom)) })
                | xflex | yflex;
            return *_focused ? pane : std::move(pane) | dim;
        }

        bool OnEvent(Event event) override
        {
            if (!_has_plan) {
                return false;
            }
            if (event.is_mouse()
                && (event.mouse().button == Mouse::WheelUp
                    || event.mouse().button == Mouse::WheelDown)) {
                return scroll_step(event, _height).has_value();
            }
            if (!*_focused) {
                return false;
            }
            if (_editor.is_open()) {
                if (event == Event::Escape) {
                    _editor.close();
                    return true;
                }
                if (is_alt_enter(event)) {
                    _editor.newline();
                    animation::RequestAnimationFrame();
                    return true;
                }
                if (event == Event::Return) {
                    _save_note();
                    return true;
                }
                return _editor.input()->OnEvent(event);
            }
            if (event.is_mouse() && event.mouse().button == Mouse::Left
                && event.mouse().motion == Mouse::Pressed) {
                return _click(event.mouse());
            }
            // The pane's keys own ' ' and Enter; the button must not see
            // them first (space_activates would fire revise on space).
            if (event == Event::ArrowUp) {
                return _move(-1);
            }
            if (event == Event::ArrowDown) {
                return _move(1);
            }
            if (event == Event::PageUp) {
                return _move(-10);
            }
            if (event == Event::PageDown) {
                return _move(10);
            }
            if (event == Event::Home) {
                _selected = 0;
                return true;
            }
            if (event == Event::End) {
                _selected = std::max(0, static_cast<int>(_rows.size()) - 1);
                return true;
            }
            if (event == Event::Character("[")) {
                return _jump_section(-1);
            }
            if (event == Event::Character("]")) {
                return _jump_section(1);
            }
            if (event == Event::Return || event == Event::Character(" ")
                || event == Event::Character("c")) {
                return _open_note();
            }
            if (event == Event::Character("e")) {
                return _edit_note();
            }
            if (event == Event::Character("d")) {
                return _delete_note();
            }
            if (event == Event::Character("s")) {
                _revise();
                return true;
            }
            if (_revise_button->OnEvent(event)) {
                return true;
            }
            return false;
        }

    private:
        struct Row {
            enum class Kind { BLOCK, NOTE };
            Kind kind         = Kind::BLOCK;
            std::size_t block = 0;
            std::size_t note  = 0;
            int section       = 0;
            int display_y     = 0;
            int height        = 1;
        };

        Element _header()
        {
            const std::string label
                = _has_plan ? plan_revision_label(_revision - 1) : "No plan";
            Elements parts {
                text("Plan") | bold | color(PANEL_FG),
                text(" · " + label) | color(PANEL_FG_DIM),
            };
            if (!_notes.empty()) {
                parts.push_back(text(std::format(" · {} notes", _notes.size()))
                    | color(HL_YELLOW));
            }
            if (!_status.empty()) {
                parts.push_back(text(" · " + _status) | color(HL_YELLOW));
            }
            return hbox(std::move(parts)) | xflex;
        }

        std::string _hint_line() const
        {
            if (!_rows.empty()) {
                const Row& row = _rows[static_cast<std::size_t>(_selected)];
                if (row.kind == Row::Kind::NOTE) {
                    return "↑↓ navigate · e edit · d delete · s revise";
                }
            }
            return "↑↓ navigate · [] sections · c note · s revise";
        }

        // Reload from the session when a plan change was signaled. The
        // signal flag plus a direct comparison keeps missed signals
        // (pane constructed after a plan existed, restored sessions)
        // converging on the session state.
        void _maybe_reload()
        {
            const bool signaled           = _on_plan_changed.exchange(false);
            const std::string session_doc = _state->session->plan_doc();
            if (!signaled && session_doc == _current) {
                return;
            }
            const SessionSnapshot snap = _state->session->snapshot();
            _has_plan                  = !snap.plans.empty();
            _revision                  = snap.plans.size();
            _current                   = session_doc;
        }

        void _build_rows(Elements& rows, int width)
        {
            _rows.clear();
            _boxes.clear();
            _blocks     = render_markdown_blocks(_current, width);
            int y       = 0;
            int section = 0;
            // The block hosting each note's card: the block containing the
            // note's line; orphaned lines fall back to the last block.
            std::vector<std::size_t> host(_notes.size(), 0);
            for (std::size_t n = 0; n < _notes.size(); ++n) {
                std::size_t best      = 0;
                int best_start        = -1;
                bool found_containing = false;
                for (std::size_t b = 0; b < _blocks.size(); ++b) {
                    const int start = _blocks[b].line;
                    const int end   = b + 1 < _blocks.size()
                        ? _blocks[b + 1].line - 1
                        : static_cast<int>(1 << 30);
                    if (start <= static_cast<int>(_notes[n].line)
                        && static_cast<int>(_notes[n].line) <= end) {
                        best             = b;
                        found_containing = true;
                        break;
                    }
                    if (start <= static_cast<int>(_notes[n].line)
                        && start > best_start) {
                        best       = b;
                        best_start = start;
                    }
                }
                host[n] = found_containing || !_blocks.empty() ? best : 0;
            }
            std::vector<int> note_at_block(_blocks.size(), -1);
            for (std::size_t n = 0; n < _notes.size(); ++n) {
                if (note_at_block[host[n]] == -1) {
                    note_at_block[host[n]] = static_cast<int>(n);
                }
            }
            const auto push = [&](Element row, Row record) {
                const int row_index = static_cast<int>(_rows.size());
                _boxes.push_back(Box { });
                if (*_focused && row_index == _selected && !_editor.is_open()) {
                    row = std::move(row) | bgcolor(PANEL_COLOR_FOCUS);
                }
                row = std::move(row) | xflex | reflect(_boxes.back());
                record.display_y = y;
                y += std::max(1, record.height);
                _rows.push_back(record);
                rows.push_back(std::move(row));
            };
            for (std::size_t b = 0; b < _blocks.size(); ++b) {
                const MarkdownBlock& block = _blocks[b];
                const bool heading         = block.source.rfind("#", 0) == 0;
                int height                 = 1;
                if (block.element) {
                    height = std::max(1, block.element->requirement().min_y);
                }
                if (heading) {
                    ++section;
                }
                push(heading ? hbox({ text("⌄ ") | color(PANEL_FG_DIM),
                                   block.element | xflex })
                             : block.element,
                    Row { Row::Kind::BLOCK, b, 0, section, 0, height });
                // Review-style inline editor: rendered in-flow right
                // under the block being annotated, not selectable.
                if (_editor.is_open()
                    && _editor.anchor() == static_cast<int>(_blocks[b].line)) {
                    const int editor_width = std::max(1, width - 15);
                    int editor_height      = 0;
                    rows.push_back(
                        annotation_editor_card(_editor.draft(), editor_width,
                            "Leave a note", &editor_height)
                        | xflex);
                    y += editor_height;
                }
                if (const int n = note_at_block[b]; n != -1) {
                    const PlanNote& note = _notes[static_cast<std::size_t>(n)];
                    const int card_width = std::max(1, width - 15);
                    int card_height      = 0;
                    Element card         = annotation_note_card(
                        note.body, card_width, &card_height);
                    push(std::move(card),
                        Row { Row::Kind::NOTE, b, static_cast<std::size_t>(n),
                            section, 0, card_height });
                }
            }
        }

        // Left press: select the hit row; a block row also opens the note
        // editor there (Review's click-to-comment). A press on the Revise
        // button takes precedence.
        bool _click(const Mouse& mouse)
        {
            if (_button_box.Contain(mouse.x, mouse.y)) {
                // Forward to the Button so it takes focus and plays its
                // animated click highlight, like Review's action buttons;
                // fall back to a direct revise if it declines.
                Mouse press;
                press.button = Mouse::Left;
                press.motion = Mouse::Pressed;
                press.x      = mouse.x;
                press.y      = mouse.y;
                if (_revise_button->OnEvent(Event::Mouse("", press))) {
                    return true;
                }
                _revise();
                return true;
            }
            for (std::size_t i = 0; i < _boxes.size(); ++i) {
                if (!_boxes[i].Contain(mouse.x, mouse.y)) {
                    continue;
                }
                _selected = static_cast<int>(i);
                if (_rows[i].kind == Row::Kind::BLOCK) {
                    return _open_note();
                }
                return true;
            }
            return false;
        }

        int _selected_note() const
        {
            if (_rows.empty() || _selected >= static_cast<int>(_rows.size())) {
                return -1;
            }
            const Row& row = _rows[static_cast<std::size_t>(_selected)];
            return row.kind == Row::Kind::NOTE ? static_cast<int>(row.note)
                                               : -1;
        }

        bool _move(int delta)
        {
            if (_rows.empty()) {
                return false;
            }
            _selected = std::clamp(
                _selected + delta, 0, static_cast<int>(_rows.size()) - 1);
            return true;
        }

        bool _jump_section(int direction)
        {
            if (_rows.empty()) {
                return false;
            }
            const int current
                = _rows[static_cast<std::size_t>(_selected)].section;
            if (direction > 0) {
                for (std::size_t i = static_cast<std::size_t>(_selected) + 1;
                    i < _rows.size(); ++i) {
                    if (_rows[i].section > current) {
                        _selected = static_cast<int>(i);
                        return true;
                    }
                }
                return false;
            }
            for (std::size_t i = static_cast<std::size_t>(_selected);
                i-- > 0;) {
                if (_rows[i].section < current) {
                    _selected = static_cast<int>(i);
                    return true;
                }
            }
            return false;
        }

        // Anchor line for a note opened on the row under the cursor: the
        // block's first source line; a heading anchors its whole section.
        bool _open_note()
        {
            if (_rows.empty()) {
                return false;
            }
            const Row& row = _rows[static_cast<std::size_t>(_selected)];
            if (row.kind != Row::Kind::BLOCK) {
                return false;
            }
            _editor.begin(_blocks[row.block].line);
            return true;
        }

        bool _edit_note()
        {
            const int n = _selected_note();
            if (n == -1) {
                return false;
            }
            _editor.begin_edit(n, _notes[static_cast<std::size_t>(n)].body);
            return true;
        }

        bool _delete_note()
        {
            const int n = _selected_note();
            if (n == -1) {
                return false;
            }
            _notes.erase(_notes.begin() + n);
            return true;
        }

        void _save_note()
        {
            const std::string body = _editor.save();
            if (body.empty()) {
                _editor.close();
                return;
            }
            if (const int id = _editor.editing_id().value_or(-1); id != -1) {
                _notes[static_cast<std::size_t>(id)].body = body;
            } else {
                _notes.push_back(
                    { static_cast<std::size_t>(_editor.anchor()), body });
                // Park the cursor on the saved card so e/d act on it.
                _pending_note_select = static_cast<int>(_notes.size()) - 1;
            }
            _editor.close();
        }

        void _revise()
        {
            if (_notes.empty()) {
                _status = "Add a note before sending.";
                _state->session->set_error(_status);
                return;
            }
            if (!_state->providers->active_selection()) {
                _status = NO_MODEL_SELECTED;
                _state->session->set_error(_status);
                return;
            }
            const std::string prompt = format_plan_annotations_prompt(
                _state->prompts->plan_annotations(), _notes, _current);
            imza::submit(*_state, prompt);
            _notes.clear();
            _editor.close();
            // A busy session queues the revise for after the running turn.
            _status = _state->session->phase() == Session::Phase::IDLE
                ? "revise turn sent"
                : "revise queued";
        }

        std::shared_ptr<ApplicationState> _state;
        LayoutFn _layout;
        const bool* _focused;
        Signal<>::Subscription _plan_subscription;
        AnnotationEditor<int> _editor;
        Component _revise_button;
        std::string _current;
        std::vector<MarkdownBlock> _blocks;
        std::vector<Row> _rows;
        std::deque<Box> _boxes;
        std::vector<PlanNote> _notes;
        Box _button_box { };
        int _selected = 0;
        // Row to select on the next render: the just-saved note card.
        int _pending_note_select = -1;
        bool _has_plan           = false;
        std::size_t _revision    = 0;
        std::string _status;
        int _height = 0;
        std::atomic<bool> _on_plan_changed { false };
    };

} // namespace

Component make_plan_doc(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, const bool* focused)
{
    return ftxui::Make<PlanDocPane>(
        std::move(state), std::move(layout), focused);
}

} // namespace imza
