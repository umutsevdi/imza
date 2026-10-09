#pragma once

#include <string>
#include <string_view>

#include "common/types.h"
#include "network/json.h"

namespace imza {

// Parse arbitrary JSON. Returns a null-typed value on malformed input.
JsonValue parse_json(std::string_view text);
std::string media_data_url(const Attachment& media);

// "content" is a plain JSON string when the message is text-only, an array
// when it carries blocks. A quoted string is its own raw JSON; anything else
// (an empty string included) embeds as null.
inline glz::raw_json raw_content(std::string_view content)
{
    const std::string quoted = json_dump(content);
    return glz::raw_json(quoted.empty() ? "null" : quoted);
}

} // namespace imza