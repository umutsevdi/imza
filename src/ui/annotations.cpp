#include "ui/annotations.h"

#include "ui/ui.h"
#include "workspace/review.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace imza {

ftxui::Element annotation_card(ftxui::Element body, int height)
{
    std::vector<ftxui::Element> rail;
    rail.reserve(static_cast<std::size_t>(std::max(1, height)));
    for (int row = 0; row < height; ++row) {
        rail.push_back(ftxui::text(" ") | ftxui::bgcolor(HL_CYAN));
    }
    ftxui::Element card
        = ftxui::hbox({ ftxui::vbox(std::move(rail)), ftxui::text(" "),
              std::move(body) | ftxui::xflex, ftxui::filler() })
        | ftxui::color(PANEL_FG) | ftxui::bgcolor(PANEL_COLOR_FOCUS);
    return ftxui::hbox(
        { ftxui::text("             "), std::move(card) | ftxui::xflex });
}

namespace {

    int wrapped_height(std::string_view body, int width)
    {
        return static_cast<int>(
            std::max<std::size_t>(1, wrap_text(body, width).size()));
    }

} // namespace

ftxui::Element annotation_note_card(
    std::string_view body, int width, int* height)
{
    ftxui::Elements rows;
    for (const std::string& line_text : wrap_text(body, width)) {
        rows.push_back(ftxui::text(line_text));
    }
    const int card_height = static_cast<int>(rows.size());
    if (height != nullptr) {
        *height = card_height;
    }
    return annotation_card(ftxui::vbox(std::move(rows)), card_height);
}

ftxui::Element annotation_editor_card(std::string_view draft, int width,
    std::string_view placeholder, int* height)
{
    const int card_height = wrapped_height(draft, width);
    if (height != nullptr) {
        *height = card_height;
    }
    return annotation_card(wrapped_input_element(draft,
                               static_cast<std::size_t>(0), width, placeholder),
        card_height);
}

template <typename Anchor> void AnnotationEditor<Anchor>::newline()
{
    insert_newline_at(_draft, _draft_cursor);
}

template class AnnotationEditor<int>;
template class AnnotationEditor<ReviewLineAnchor>;

} // namespace imza
