#include "ui/annotations.h"

#include "ui/ui.h"
#include "workspace/review.h"

#include <algorithm>
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

template <typename Anchor> void AnnotationEditor<Anchor>::newline()
{
    insert_newline_at(draft_, draft_cursor_);
}

template class AnnotationEditor<int>;
template class AnnotationEditor<ReviewLineAnchor>;

} // namespace imza
