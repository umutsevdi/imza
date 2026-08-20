#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <string_view>

namespace ursa {

ftxui::Element render_markdown_element(std::string_view md);

} // namespace ursa
