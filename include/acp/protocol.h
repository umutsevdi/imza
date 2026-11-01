#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "acp/wire.h"
#include "common/modal.h"
#include "common/permission_request.h"
#include "conversation/session.h"

namespace imza::acp {

// Imza executes locally, so client fs/terminal capabilities stay off.
struct ClientCapabilities {
    bool fs_read_text_file = false;
    bool terminal          = false;
    bool elicitation_form  = false;

    static ClientCapabilities parse(const JsonValue& params);
};

struct InitializeRequest {
    int protocol_version = 1;
    ClientCapabilities client;
    std::string client_name;

    static InitializeRequest parse(const JsonValue& params);
};

JsonValue dump_initialize_result(const InitializeRequest& request);

enum class StopReason { END_TURN, MAX_TOKENS, REFUSAL, CANCELLED };
std::string_view stop_reason_name(StopReason reason);

struct PromptContent {
    enum class Kind { TEXT, IMAGE, RESOURCE, RESOURCE_LINK, OTHER };
    Kind kind = Kind::TEXT;
    std::string text;
    std::string media_type;
    std::string data;
};

struct PromptRequest {
    std::string session_id;
    std::vector<PromptContent> prompt;

    static PromptRequest parse(const JsonValue& params);
};

enum class ToolKind {
    READ,
    EDIT,
    DELETE,
    MOVE,
    SEARCH,
    EXECUTE,
    THINK,
    FETCH,
    SWITCH_MODE,
    OTHER
};
ToolKind tool_kind_for(std::string_view tool_name, const ToolCall& call);
std::string_view tool_kind_name(ToolKind kind);

struct PermissionOption {
    std::string id;
    std::string name;
    std::string kind;
};

// The ACP view of one permission modal.
struct PermissionRequest {
    std::string session_id;
    std::string tool_call_id;
    std::vector<PermissionOption> options;
    JsonValue tool_call_update;
};

// allow_always appears only when a session grant can express it.
std::vector<PermissionOption> permission_options(
    const PermissionPrompt& prompt);
// Nullopt for payloads that are not permission requests.
std::optional<PermissionRequest> permission_request_for(
    std::string_view session_id, const PermissionPrompt& prompt);

// The picked option's kind, or "" for cancelled/unrecognized.
std::string permission_outcome_kind(const JsonValue& result);

struct ConfigOption {
    std::string id;
    std::string name;
    std::string category; // "mode" | "model" | "thought_level"
    std::string current_value;
    std::vector<std::string> values;
};

std::vector<ConfigOption> config_options(const std::string& mode,
    const std::string& model, const std::string& effort);
JsonValue dump_config_options(const std::vector<ConfigOption>& options);

JsonValue available_command(
    std::string_view name, std::string_view description);

} // namespace imza::acp
