#pragma once

#include <string_view>

#include "workspace/environment.h"

namespace imza {

bool copy_to_clipboard(
    const SystemEnvironment& environment, std::string_view text);

} // namespace imza
