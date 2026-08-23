#include "ui.h"

#include <ftxui/dom/elements.hpp>
#include <optional>

namespace ursa {

using namespace ftxui;

Element panel(Element e)
{
    return std::move(e) | bgcolor(PANEL_COLOR) | color(PANEL_FG);
}

Element card(Element body, std::optional<Color> bg)
{
    Element inner = vbox({ separatorEmpty(), std::move(body), separatorEmpty() });
    Element box = hbox({ text("  "), std::move(inner) | xflex, text("  ") });
    if (bg) {
        box = std::move(box) | bgcolor(*bg) | color(PANEL_FG);
    }
    return std::move(box) | xflex;
}

Element section_title(std::string_view title, Color fg)
{
    return text(std::string(title)) | bold | color(fg);
}

Element code_block(const std::string& code)
{
    const Color bg = Color::Palette256(234);
    const Color fg = Color::Palette256(245);
    Elements body;
    body.push_back(separatorEmpty());
    size_t pos = 0;
    for (;;) {
        const size_t nl = code.find('\n', pos);
        const std::string line
            = code.substr(pos, nl == std::string::npos ? nl : nl - pos);
        body.push_back(text(line) | color(fg));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    body.push_back(separatorEmpty());
    Element inner = vbox(std::move(body));
    return hbox({ text(" "), std::move(inner) | xflex, text(" ") }) | bgcolor(bg);
}

} // namespace ursa
