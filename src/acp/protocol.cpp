#include "acp/protocol.h"

#include "platform/config.h"

#include <algorithm>
#include <numeric>

namespace imza::acp {

namespace {

    constexpr std::string_view ALLOW_ONCE    = "allow_once";
    constexpr std::string_view ALLOW_ALWAYS  = "allow_always";
    constexpr std::string_view REJECT_ONCE   = "reject_once";
    constexpr std::string_view REJECT_ALWAYS = "reject_always";

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
    // Form mode is advertised only by a non-null form field; a bare {}
    // or all-null object advertises nothing (differs from MCP). URL mode
    // has no imza flow yet, so it is not parsed.
    if (const JsonValue* elicitation = find_member(params, "elicitation");
        elicitation != nullptr && elicitation->is_object()) {
        const JsonValue* form = find_member(*elicitation, "form");
        out.elicitation_form  = form != nullptr && !form->is_null();
    }
    // A present (non-null) object advertises; omitted or null means
    // unsupported. The inner object is empty: it carries no fields yet.
    const auto gated = [&](std::string_view outer, std::string_view inner) {
        const JsonValue* group = find_member(params, outer);
        if (group == nullptr || !group->is_object()) {
            return false;
        }
        const JsonValue* leaf = find_member(*group, inner);
        return leaf != nullptr && !leaf->is_null();
    };
    out.compaction = gated("session", "compaction");
    out.notices    = gated("session", "notices");
    // Nested one level deeper: session.configOptions.boolean.
    if (const JsonValue* session = find_member(params, "session");
        session != nullptr && session->is_object()) {
        if (const JsonValue* options = find_member(*session, "configOptions");
            options != nullptr && options->is_object()) {
            const JsonValue* boolean = find_member(*options, "boolean");
            out.boolean_options = boolean != nullptr && !boolean->is_null();
        }
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
    // The requested version when supported, otherwise the latest version
    // imza speaks; a client that cannot match disconnects.
    result["protocolVersion"]
        = static_cast<double>(request.protocol_version == ACP_PROTOCOL_VERSION
                ? request.protocol_version
                : ACP_PROTOCOL_VERSION);
    JsonValue agent;
    agent["loadSession"] = true;
    JsonValue prompt;
    // Images and resource blocks are folded into prompts; imza never
    // echoes media back, so nothing here is about agent output.
    prompt["image"]             = true;
    prompt["audio"]             = false;
    prompt["embeddedContext"]   = true;
    agent["promptCapabilities"] = prompt;
    // The session's permission store accepts extra directory grants, so
    // clients may widen the root set at session setup.
    JsonValue session_capabilities;
    session_capabilities["list"]                  = json_object();
    session_capabilities["delete"]                = json_object();
    session_capabilities["resume"]                = json_object();
    session_capabilities["close"]                 = json_object();
    session_capabilities["additionalDirectories"] = json_object();
    agent["sessionCapabilities"] = std::move(session_capabilities);
    JsonValue mcp;
    // The manager speaks streamable HTTP; stdio is mandatory for agents.
    mcp["http"]                 = true;
    mcp["sse"]                  = false;
    agent["mcpCapabilities"]    = mcp;
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
        } else if (kind == "resource") {
            // Text resources carry their content in text; blob resources
            // in data. The uri names the source either way.
            content.kind = PromptContent::Kind::RESOURCE;
            content.uri  = string_member(block, "uri");
            if (const JsonValue* resource = find_member(block, "resource");
                resource != nullptr && resource->is_object()) {
                content.uri        = string_member(*resource, "uri");
                content.text       = string_member(*resource, "text");
                content.data       = string_member(*resource, "blob");
                content.media_type = string_member(*resource, "mimeType");
            }
        } else if (kind == "resource_link") {
            content.kind = PromptContent::Kind::RESOURCE_LINK;
            content.uri  = string_member(block, "uri");
            content.name = string_member(block, "name");
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
    case ToolKind::EXECUTE: return "execute";
    case ToolKind::THINK: return "think";
    case ToolKind::FETCH: return "fetch";
    case ToolKind::OTHER: return "other";
    }
    return "other";
}

std::optional<PermissionRequest> permission_request_for(
    std::string_view session_id, const PermissionPrompt& prompt)
{
    PermissionRequest out;
    out.session_id   = std::string(session_id);
    out.tool_call_id = prompt.id.empty() ? "manual" : prompt.id;
    // allow_always / reject_always appear only when a session grant can
    // express them.
    out.options.push_back(
        { "allow-once", "Allow once", std::string(ALLOW_ONCE) });
    if (prompt.allow_for_session) {
        out.options.push_back({ "allow-session", "Allow for this session",
            std::string(ALLOW_ALWAYS) });
        out.options.push_back({ "reject-session", "Reject for this session",
            std::string(REJECT_ALWAYS) });
    }
    out.options.push_back(
        { "reject-once", "Reject", std::string(REJECT_ONCE) });
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
    if (option == "reject-session" || option == REJECT_ALWAYS) {
        return std::string(REJECT_ALWAYS);
    }
    return "";
}

bool permission_outcome_rejects_always(const JsonValue& result)
{
    return permission_outcome_kind(result) == REJECT_ALWAYS;
}

std::vector<ConfigOption> config_options(const std::string& mode,
    const std::string& model, const std::string& effort,
    const std::vector<std::pair<std::string, std::string>>& models,
    std::optional<bool> skip_permissions)
{
    std::vector<ConfigOption> options;
    ConfigOption mode_option;
    mode_option.id   = "mode";
    mode_option.name = "Workflow mode";
    mode_option.description
        = "Plan researches without editing; Build writes code";
    mode_option.category      = "mode";
    mode_option.type          = "select";
    mode_option.current_value = mode;
    mode_option.values = { { "plan", "Plan",
                               "Research and design without modifying files" },
        { "build", "Build", "Write and modify code with full tool access" } };
    options.push_back(std::move(mode_option));
    if (!models.empty()) {
        ConfigOption model_option;
        model_option.id            = "model";
        model_option.name          = "Model";
        model_option.category      = "model";
        model_option.type          = "select";
        model_option.current_value = model;
        for (const auto& [id, label] : models) {
            model_option.values.push_back({ id, label, "" });
        }
        options.push_back(std::move(model_option));
    }
    ConfigOption effort_option;
    effort_option.id            = "thought_level";
    effort_option.name          = "Reasoning effort";
    effort_option.category      = "thought_level";
    effort_option.type          = "select";
    effort_option.current_value = effort;
    effort_option.values        = { { "off", "Off", "" }, { "low", "Low", "" },
        { "default", "Default", "" }, { "high", "High", "" } };
    options.push_back(std::move(effort_option));
    if (skip_permissions.has_value()) {
        ConfigOption auto_option;
        auto_option.id   = "skip_permissions";
        auto_option.name = "Auto Mode";
        auto_option.description
            = "Skip confirmation prompts and act autonomously";
        auto_option.category     = "mode";
        auto_option.type         = "boolean";
        auto_option.current_bool = *skip_permissions;
        options.push_back(std::move(auto_option));
    }
    return options;
}

JsonValue dump_config_options(const std::vector<ConfigOption>& options)
{
    JsonValue::array_t array;
    for (const ConfigOption& option : options) {
        JsonValue entry;
        entry["id"]   = option.id;
        entry["name"] = option.name;
        if (!option.description.empty()) {
            entry["description"] = option.description;
        }
        entry["category"] = option.category;
        entry["type"]     = option.type.empty() ? "select" : option.type;
        if (option.type == "boolean") {
            entry["currentValue"] = option.current_bool;
        } else {
            entry["currentValue"] = option.current_value;
            JsonValue::array_t values;
            for (const ConfigValue& value : option.values) {
                JsonValue item;
                item["value"] = value.value;
                item["name"]  = value.name;
                if (!value.description.empty()) {
                    item["description"] = value.description;
                }
                values.push_back(std::move(item));
            }
            entry["options"] = std::move(values);
        }
        array.push_back(std::move(entry));
    }
    return JsonValue(std::move(array));
}

JsonValue dump_session_modes(const std::string& current_mode)
{
    JsonValue::array_t modes;
    for (const auto& [id, name, description] :
        { std::tuple {
              "plan", "Plan", "Research and design without modifying files" },
            std::tuple { "build", "Build",
                "Write and modify code with full tool access" } }) {
        JsonValue mode;
        mode["id"]          = std::string(id);
        mode["name"]        = std::string(name);
        mode["description"] = std::string(description);
        modes.push_back(std::move(mode));
    }
    JsonValue state;
    state["currentModeId"]  = current_mode;
    state["availableModes"] = std::move(modes);
    return state;
}

JsonValue available_command(std::string_view name, std::string_view description,
    std::string_view input_hint)
{
    JsonValue entry;
    entry["name"]        = std::string(name);
    entry["description"] = std::string(description);
    if (!input_hint.empty()) {
        JsonValue input;
        input["hint"]  = std::string(input_hint);
        entry["input"] = std::move(input);
    }
    return entry;
}

} // namespace imza::acp
