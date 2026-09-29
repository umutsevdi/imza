#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <ftxui/component/component_options.hpp>

#include <functional>
#include <optional>
#include <string>

namespace imza {

ftxui::InputOption multiline_field_option(std::string* content, int* cursor,
    std::string placeholder, std::function<void()> on_change);

// Review-style annotation card: cyan rail, focused background, body rows.
ftxui::Element annotation_card(ftxui::Element body, int height);

// Inline draft editor for annotation cards: the multiline input, the draft
// buffer, and the new-versus-edit state. The host pane decides when to
// open, save, and cancel; it applies the note using anchor() and
// editing_id() after save(). The anchor type is the pane's locator - a
// document line for the plan annotator, a diff line anchor for Review.
template <typename Anchor> class AnnotationEditor {
public:
    explicit AnnotationEditor(std::function<void()> on_change = { })
        : input_(ftxui::Input(&draft_,
              multiline_field_option(&draft_, &draft_cursor_, "Leave a comment",
                  std::move(on_change))))
    {
    }

    ftxui::Component input() const { return input_; }
    bool is_open() const { return open_; }
    const std::string& draft() const { return draft_; }
    const Anchor& anchor() const { return anchor_; }
    // Set when editing an existing note; nullopt for a new note.
    std::optional<int> editing_id() const { return editing_; }

    void begin(const Anchor& anchor)
    {
        anchor_ = anchor;
        editing_.reset();
        open_ = true;
        draft_.clear();
        draft_cursor_ = 0;
        input_->TakeFocus();
    }

    void begin_edit(int id, std::string body)
    {
        editing_      = id;
        open_         = true;
        draft_        = std::move(body);
        draft_cursor_ = static_cast<int>(draft_.size());
        input_->TakeFocus();
    }

    // Closes the editor and returns the draft; empty means nothing to save.
    std::string save()
    {
        open_ = false;
        return std::exchange(draft_, { });
    }

    void close()
    {
        open_ = false;
        editing_.reset();
        draft_.clear();
        draft_cursor_ = 0;
    }

    void newline();

private:
    std::string draft_;
    int draft_cursor_ = 0;
    Anchor anchor_ { };
    std::optional<int> editing_;
    bool open_ = false;
    ftxui::Component input_;
};

} // namespace imza
