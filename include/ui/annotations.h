#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <ftxui/component/component_options.hpp>

#include <functional>
#include <optional>
#include <string>

namespace imza {

ftxui::InputOption multiline_field_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change);

// Note card over `body` wrapped to `width`; *height receives the wrapped
// row count for the host's row bookkeeping.
ftxui::Element annotation_note_card(
    std::string_view body, int width, int* height);
// Editor card showing `draft` as a wrapped, cursor-less input with
// `placeholder`; *height as above.
ftxui::Element annotation_editor_card(std::string_view draft, int width,
    std::string_view placeholder, int* height);

// What a host pane should do after the open editor handled a key.
enum class AnnotationAction {
    NONE,    // key not consumed; the host forwards it to input()
    HANDLED, // consumed; the host just returns true
    SAVE,
    CANCEL,
};

// Inline draft editor for annotation cards: the multiline input, the draft
// buffer, and the new-versus-edit state. The host pane decides when to
// open, save, and cancel; it applies the note using anchor() and
// editing_id() after save(). The anchor type is the pane's locator - a
// document line for the plan annotator, a diff line anchor for Review.
template <typename Anchor> class AnnotationEditor {
public:
    explicit AnnotationEditor(std::function<void()> on_change = { })
        : _input(ftxui::Input(&_draft,
              multiline_field_option(&_draft, &_draft_cursor, "Leave a comment",
                  std::move(on_change))))
    {
    }

    ftxui::Component input() const { return _input; }
    bool is_open() const { return _open; }
    const std::string& draft() const { return _draft; }
    const Anchor& anchor() const { return _anchor; }
    // Set when editing an existing note; nullopt for a new note.
    std::optional<int> editing_id() const { return _editing; }

    void begin(const Anchor& anchor)
    {
        _anchor = anchor;
        _editing.reset();
        _open = true;
        _draft.clear();
        _draft_cursor = 0;
        _input->TakeFocus();
    }

    void begin_edit(int id, std::string body)
    {
        _editing      = id;
        _open         = true;
        _draft        = std::move(body);
        _draft_cursor = static_cast<int>(_draft.size());
        _input->TakeFocus();
    }

    // Closes the editor and returns the draft; empty means nothing to save.
    std::string save()
    {
        _open = false;
        return std::exchange(_draft, { });
    }

    void close()
    {
        _open = false;
        _editing.reset();
        _draft.clear();
        _draft_cursor = 0;
    }

    void newline();

    // Shared open-editor keys: Esc cancels, Alt+Enter inserts a newline,
    // Return saves. NONE means the key was not consumed and the host
    // forwards it to input().
    AnnotationAction handle_event(const ftxui::Event& event);

private:
    std::string _draft;
    int _draft_cursor = 0;
    Anchor _anchor { };
    std::optional<int> _editing;
    bool _open = false;
    ftxui::Component _input;
};

} // namespace imza
