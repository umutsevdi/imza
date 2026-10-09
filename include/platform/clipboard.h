#pragma once

#include <string_view>

namespace imza {

// Copies `text` via the platform clipboard tool. An empty tool or text is a
// no-op. The tool name comes from the caller (the workspace environment) so
// this foundational layer does not depend on the workspace domain.
bool copy_to_clipboard(std::string_view clipboard_tool, std::string_view text);

} // namespace imza
