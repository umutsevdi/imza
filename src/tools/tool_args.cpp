#include "common/util.h"
#include "network/json_io.h"
#include "tools/tool.h"

namespace imza {

namespace {

    std::optional<std::string> validate_positive(std::string_view tool,
        const Json::Value& arguments, const char* key, bool allow_zero)
    {
        if (!arguments.isMember(key)) {
            return std::nullopt;
        }
        const Json::Value& value = arguments[key];
        if (!value.isInt64()
            || value.asInt64() < static_cast<Json::Int64>(allow_zero ? 0 : 1)) {
            return std::string(tool) + ": " + key
                + (allow_zero ? " must be 0 or greater"
                              : " must be 1 or greater");
        }
        return std::nullopt;
    }

} // namespace

std::optional<std::string> validate_filesystem_tool_arguments(
    std::string_view tool, const Json::Value& arguments)
{
    if (!arguments.isObject()) {
        return std::string(tool) + ": arguments must be an object";
    }
    const bool write = tool == "edit" || tool == "write" || tool == "insert";
    const char* key  = write ? "file_path" : "path";
    if (tool == "list" || tool == "find") {
        if (arguments.isMember(key) && !arguments[key].isNull()
            && !arguments[key].isString()) {
            return std::string(tool) + ": path must be a string";
        }
    } else if (!arguments[key].isString()
        || arguments[key].asString().empty()) {
        return std::string(tool) + ": " + key + " must be a non-empty string";
    }
    if (tool == "read") {
        if (auto error
            = validate_positive(tool, arguments, "line_begin", false)) {
            return error;
        }
        if (auto error
            = validate_positive(tool, arguments, "line_end", false)) {
            return error;
        }
        if (arguments.isMember("line_begin") && arguments.isMember("line_end")
            && arguments["line_end"].asInt64()
                < arguments["line_begin"].asInt64()) {
            return "read: line_end is before line_begin";
        }
        return std::nullopt;
    }
    if (tool == "list") {
        return std::nullopt;
    }
    if (tool == "find") {
        if (!arguments["pattern"].isString()
            || arguments["pattern"].asString().empty()) {
            return "find: pattern must be a non-empty string";
        }
        return std::nullopt;
    }
    if (tool == "edit") {
        if (!arguments["old_string"].isString()
            || arguments["old_string"].asString().empty()) {
            return "edit: old_string must be a non-empty string";
        }
        if (!arguments["new_string"].isString()) {
            return "edit: new_string must be a string";
        }
        return validate_positive(tool, arguments, "replace_count", true);
    }
    if (tool == "insert") {
        if (!arguments["text"].isString()) {
            return "insert: text must be a string";
        }
        return validate_positive(tool, arguments, "line", true);
    }
    if (tool != "write") {
        return std::string(tool) + ": unsupported filesystem tool";
    }
    if (!arguments["text"].isString()) {
        return "write: text must be a string";
    }
    return std::nullopt;
}

std::optional<std::string> validate_shell_tool_arguments(
    const Json::Value& arguments)
{
    if (!arguments.isObject() || !arguments["command"].isString()
        || arguments["command"].asString().empty()) {
        return "shell: 'command' must be a non-empty string";
    }
    if (arguments.isMember("timeout")
        && (!arguments["timeout"].isInt64()
            || arguments["timeout"].asInt64() < 1)) {
        return "shell: timeout must be 1 or greater";
    }
    return std::nullopt;
}

std::optional<std::string> validate_subagent_tool_arguments(
    const Json::Value& arguments, bool allow_build)
{
    if (!arguments.isObject() || !arguments["tasks"].isArray()
        || arguments["tasks"].empty() || arguments["tasks"].size() > 5) {
        return "subagent: expected one to five tasks";
    }
    for (const Json::Value& value : arguments["tasks"]) {
        if (!value.isObject() || !value["mode"].isString()
            || !value["prompt"].isString()
            || trim(value["prompt"].asString()).empty()) {
            return "subagent: every task requires a mode and prompt";
        }
        const std::string mode = to_lower(value["mode"].asString());
        if (mode != "research" && mode != "build") {
            return "subagent: mode must be research or build";
        }
        if (mode == "build" && !allow_build) {
            return "subagent: build agents require main-agent build mode";
        }
    }
    return std::nullopt;
}

std::string json_string(const Json::Value& value, const char* key)
{
    return value.isObject() && value[key].isString() ? value[key].asString()
                                                     : std::string { };
}

std::optional<std::int64_t> json_int(const Json::Value& value, const char* key)
{
    if (!value.isObject() || !value[key].isIntegral()) {
        return std::nullopt;
    }
    return value[key].asInt64();
}

} // namespace imza
