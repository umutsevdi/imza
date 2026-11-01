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
    bool elicitation_url   = false;
    // Compaction updates and notices are capability-gated: the adapter
    // must not send either without the matching client advertisement.
    bool compaction = false;
    bool notices    = false;
    // session.configOptions.boolean: boolean config options allowed.
    bool boolean_options = false;

    static ClientCapabilities parse(const JsonValue& params);
};

inline constexpr int ACP_PROTOCOL_VERSION = 1;

struct InitializeRequest {
    // The version the client asked for; dump_initialize_result echoes it
    // when supported, otherwise the latest version imza speaks.
    int protocol_version = ACP_PROTOCOL_VERSION;
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
    // resource blocks: the resource uri (text content rides in `text`,
    // blob content in `data`); resource_link blocks: uri, name, and the
    // optional display fields.
    std::string uri;
    std::string name;
    std::string title;
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
// True when the picked option asked to remember a rejection; the caller
// keeps the tool blocked for the session without re-asking.
bool permission_outcome_rejects_always(const JsonValue& result);

struct ConfigValue {
    std::string value;
    std::string name;
    std::string description;
};

struct ConfigOption {
    std::string id;
    std::string name;
    std::string description;
    std::string category; // "mode" | "model" | "thought_level"
    // "select" (default) or "boolean"; boolean requires the client's
    // session.configOptions.boolean advertisement.
    std::string type;
    std::string current_value;
    bool current_bool = false;
    std::vector<ConfigValue> values;
};

std::vector<ConfigOption> config_options(const std::string& mode,
    const std::string& model, const std::string& effort,
    const std::vector<std::pair<std::string, std::string>>& models = { },
    std::optional<bool> skip_permissions = std::nullopt);
JsonValue dump_config_options(const std::vector<ConfigOption>& options);

// The legacy SessionModeState, still sent alongside configOptions for
// clients that only understand session/set_mode.
JsonValue dump_session_modes(const std::string& current_mode);

JsonValue available_command(std::string_view name, std::string_view description,
    std::string_view input_hint = { });

} // namespace imza::acp
