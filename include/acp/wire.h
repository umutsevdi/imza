#pragma once

#include <string>
#include <string_view>

#include "network/json_rpc.h"

namespace imza::acp {

// ACP's frame is the shared JSON-RPC 2.0 frame; the protocol adds only
// its own error codes and payload builders below.
using Message = RpcFrame;

// ACP-specific error codes (the standard JSON-RPC ones live in
// network/json_rpc.h).
inline constexpr int ERR_RESOURCE_NOT_FOUND = -32002;

// session/update payload builders.
JsonValue text_block(std::string_view text);
JsonValue session_update(std::string_view session_id, JsonValue update);
JsonValue update_of(std::string_view kind);

} // namespace imza::acp
