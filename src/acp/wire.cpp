#include "acp/wire.h"

namespace imza::acp {

JsonValue text_block(std::string_view text)
{
    JsonValue block;
    block["type"] = "text";
    block["text"] = std::string(text);
    return block;
}

JsonValue session_update(std::string_view session_id, JsonValue update)
{
    JsonValue params;
    params["sessionId"] = std::string(session_id);
    params["update"]    = std::move(update);
    return params;
}

JsonValue update_of(std::string_view kind)
{
    JsonValue update;
    update["sessionUpdate"] = std::string(kind);
    return update;
}

} // namespace imza::acp
