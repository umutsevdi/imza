#include "acp/protocol.h"

#include "platform/config.h"

#include <algorithm>
#include <numeric>

namespace imza::acp {

namespace {

    constexpr std::string_view ALLOW_ONCE   = "allow_once";
    constexpr std::string_view ALLOW_ALWAYS = "allow_always";
    constexpr std::string_view REJECT_ONCE  = "reject_once";

    std::optional<bool> bool_member(
        const JsonValue& object, std::string_view outer, std::string_view inner)
    {
        const JsonValue* group = find_member(object, outer);
        if (group == nullptr || !group->is_object()) {
            return std::nullopt;
        }
        if (inner.empty()) {
            if (!group->is_boolean()) {
                return std::nullopt;
            }
            return group->get<bool>();
        }
        const JsonValue* value = find_member(*group, inner);
        if (value == nullptr || !value->is_boolean()) {
            return std::nullopt;
        }
        return value->get<bool>();
    }

    std::string string_member(const JsonValue& object, std::string_view key)
    {
        const JsonValue* value = find_member(object, key);
        if (value == nullptr || !value->is_string()) {
            return { };
        }
        return value->get<std::string>();
    }

} // namespace

ClientCapabilities ClientCapabilities::parse(const JsonValue& params)
{
    ClientCapabilities out;
    out.fs_read_text_file
        = bool_member(params, "fs", "readTextFile").value_or(false);
    out.terminal = bool_member(params, "terminal", "").value_or(false);
    const JsonValue* elicitation = find_member(params, "elicitation");
    if (elicitation != nullptr && elicitation->is_object()) {
        out.elicitation_form = find_member(*elicitation, "form") != nullptr;
    }
    return out;
}

InitializeRequest InitializeRequest::parse(const JsonValue& params)
{
    InitializeRequest out;
    if (const JsonValue* version = find_member(params, "protocolVersion");
        version != nullptr && version->is_number()) {
        out.protocol_version = static_cast<int>(version->get<double>());
    }
    if (const JsonValue* client = find_member(params, "clientCapabilities");
        client != nullptr && client->is_object()) {
        out.client = ClientCapabilities::parse(*client);
    }
    if (const JsonValue* info = find_member(params, "clientInfo");
        info != nullptr && info->is_object()) {
        out.client_name = string_member(*info, "name");
    }
    return out;
}

JsonValue dump_initialize_result(const InitializeRequest& request)
{
    JsonValue result;
    result["protocolVersion"] = static_cast<double>(request.protocol_version);
    JsonValue agent;
    agent["loadSession"] = true;
    JsonValue prompt;
    // Imza folds client images into prompts but never echoes media back.
    prompt["image"]             = false;
    prompt["audio"]             = false;
    prompt["embeddedContext"]   = false;
    agent["promptCapabilities"] = prompt;
    result["agentCapabilities"] = agent;
    JsonValue info;
    info["name"]        = "imza";
    info["title"]       = "Imza";
    info["version"]     = std::string(IMZA_VERSION);
    result["agentInfo"] = info;
    // No authMethods: providers connect through the TUI (/connect) or
    // config; advertising terminal auth makes clients relaunch imza for
    // an interactive login instead of starting the session.
    return result;
}

std::string_view stop_reason_name(StopReason reason)
{
    switch (reason) {
    case StopReason::END_TURN: return "end_turn";
    case StopReason::MAX_TOKENS: return "max_tokens";
    case StopReason::REFUSAL: return "refusal";
    case StopReason::CANCELLED: return "cancelled";
    }
    return "end_turn";
}

PromptRequest PromptRequest::parse(const JsonValue& params)
{
    PromptRequest out;
    out.session_id          = string_member(params, "sessionId");
    const JsonValue* prompt = find_member(params, "prompt");
    if (prompt == nullptr || !prompt->is_array()) {
        return out;
    }
    for (const JsonValue& block : prompt->get<JsonValue::array_t>()) {
        PromptContent content;
        const std::string kind = string_member(block, "type");
        if (kind.empty() || kind == "text") {
            content.kind = PromptContent::Kind::TEXT;
            content.text = string_member(block, "text");
        } else if (kind == "image") {
            content.kind       = PromptContent::Kind::IMAGE;
            content.data       = string_member(block, "data");
            content.media_type = string_member(block, "mimeType");
        } else {
            content.kind = PromptContent::Kind::OTHER;
        }
        out.prompt.push_back(std::move(content));
    }
    return out;
}

namespace {

    // Imza works through one lua execution layer; the wire call is the
    // whole run. While it is still planning, execute is the honest
    // placeholder for what the script will do.
    ToolKind classify(std::string_view name, bool planning)
    {
        if (name == "load_skill") {
            return ToolKind::THINK;
        }
        if (name == "load_module") {
            return ToolKind::READ;
        }
        if (name == "load_mcp") {
            return ToolKind::FETCH;
        }
        if (name == "subagent") {
            return ToolKind::THINK;
        }
        if (name == "lua") {
            return planning ? ToolKind::EXECUTE : ToolKind::OTHER;
        }
        return ToolKind::OTHER;
    }

} // namespace

ToolKind tool_kind_for(std::string_view tool_name, const ToolCall& call)
{
    return classify(tool_name, call.phase == ToolCall::Phase::PLANNING);
}

std::string_view tool_kind_name(ToolKind kind)
{
    switch (kind) {
    case ToolKind::READ: return "read";
    case ToolKind::EDIT: return "edit";
    case ToolKind::DELETE: return "delete";
    case ToolKind::MOVE: return "move";
    case ToolKind::SEARCH: return "search";
    case ToolKind::EXECUTE: return "execute";
    case ToolKind::THINK: return "think";
    case ToolKind::FETCH: return "fetch";
    case ToolKind::SWITCH_MODE: return "switch_mode";
    case ToolKind::OTHER: return "other";
    }
    return "other";
}

std::vector<PermissionOption> permission_options(const PermissionPrompt& prompt)
{
    std::vector<PermissionOption> options;
    options.push_back({ "allow-once", "Allow once", std::string(ALLOW_ONCE) });
    if (prompt.allow_for_session) {
        options.push_back({ "allow-session", "Allow for this session",
            std::string(ALLOW_ALWAYS) });
    }
    options.push_back({ "reject-once", "Reject", std::string(REJECT_ONCE) });
    return options;
}

std::optional<PermissionRequest> permission_request_for(
    std::string_view session_id, const PermissionPrompt& prompt)
{
    PermissionRequest out;
    out.session_id       = std::string(session_id);
    out.tool_call_id     = prompt.id.empty() ? "manual" : prompt.id;
    out.options          = permission_options(prompt);
    JsonValue update     = update_of("tool_call_update");
    update["toolCallId"] = out.tool_call_id;
    update["title"]      = prompt.name;
    update["status"]     = "pending";
    update["rawInput"]   = prompt.description;
    if (!prompt.target.empty()) {
        update["locations"] = JsonValue::array_t { JsonValue(prompt.target) };
    }
    out.tool_call_update = std::move(update);
    return out;
}

std::string permission_outcome_kind(const JsonValue& result)
{
    const JsonValue* outcome = find_member(result, "outcome");
    if (outcome == nullptr || !outcome->is_object()) {
        return "";
    }
    const JsonValue* kind = find_member(*outcome, "outcome");
    if (kind == nullptr || !kind->is_string()
        || kind->get<std::string>() != "selected") {
        return "";
    }
    const std::string option = string_member(*outcome, "optionId");
    if (option == "allow-once" || option == ALLOW_ONCE) {
        return std::string(ALLOW_ONCE);
    }
    if (option == "allow-session" || option == ALLOW_ALWAYS) {
        return std::string(ALLOW_ALWAYS);
    }
    if (option == "reject-once" || option == REJECT_ONCE) {
        return std::string(REJECT_ONCE);
    }
    return "";
}

std::vector<ConfigOption> config_options(const std::string& mode,
    const std::string& model, const std::string& effort)
{
    std::vector<ConfigOption> options;
    options.push_back(
        { "mode", "Workflow mode", "mode", mode, { "plan", "build" } });
    options.push_back({ "model", "Model", "model", model, { } });
    options.push_back({ "thought_level", "Reasoning effort", "thought_level",
        effort, { "off", "low", "default", "high" } });
    return options;
}

JsonValue dump_config_options(const std::vector<ConfigOption>& options)
{
    JsonValue::array_t array;
    for (const ConfigOption& option : options) {
        JsonValue entry;
        entry["id"]           = option.id;
        entry["name"]         = option.name;
        entry["category"]     = option.category;
        entry["type"]         = "select";
        entry["currentValue"] = option.current_value;
        JsonValue::array_t values;
        if (option.values.empty()) {
            // Open-ended options (model) expose the current selection;
            // the model catalog stays internal.
            values.push_back(option.current_value);
        } else {
            for (const std::string& value : option.values) {
                values.push_back(JsonValue(value));
            }
        }
        entry["options"] = std::move(values);
        array.push_back(std::move(entry));
    }
    return JsonValue(std::move(array));
}

JsonValue available_command(std::string_view name, std::string_view description)
{
    JsonValue entry;
    entry["name"]        = std::string(name);
    entry["description"] = std::string(description);
    return entry;
}

} // namespace imza::acp
