#include "network/json_io.h"

#include "common/util.h"

namespace imza {

JsonValue parse_json(std::string_view text)
{
    JsonValue value;
    if (text.empty() || json_parse_checked(text, value)) {
        return JsonValue { };
    }
    return value;
}

std::string media_data_url(const Attachment& media)
{
    return "data:" + media.media_type + ";base64," + media.encoded_or_compute();
}

} // namespace imza
