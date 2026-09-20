#pragma once

#include <json/json.h>

#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/diff.h"
#include "common/tool_call.h"
#include "common/types.h"
#include "tools/lua.h"

namespace imza {

struct ToolOutput {
    enum class Kind { OUTPUT, ERROR };
    Kind kind;
    std::string text;
    // Net per-file diffs a lua script produced through tool.file.*;
    // multiple mutations of one file collapse into a single before/after.
    std::vector<DiffView> diffs { };
    std::vector<LuaBindingCall> dispatch_log { };
    bool blocked_permission = false;
};

inline ToolOutput tool_error(std::string text)
{
    return { ToolOutput::Kind::ERROR, std::move(text) };
}

using ToolHandler = std::function<ToolOutput(const Json::Value& args)>;

struct Tool {
    ToolSpec spec;
    ToolHandler run;
};

const Tool* find_tool(std::span<const Tool> tools, std::string_view name);
std::vector<ToolSpec> tool_specs(std::span<const Tool> tools);
ToolOutput dispatch_tool(
    std::span<const Tool> tools, const ToolCallRequest& req);

// Argument accessors: "" / nullopt unless `value[key]` holds that type.
std::string json_string(const Json::Value& value, const char* key);
std::optional<std::int64_t> json_int(const Json::Value& value, const char* key);

std::optional<std::string> validate_subagent_tool_arguments(
    const Json::Value& arguments, bool allow_build);

Tool make_skill_tool();
Tool make_subagent_tool();

// Builds the model-facing roster: skill, subagent, and the lua sandbox
// (see tools/lua.h for the LuaHost the lua tool is wired with).
std::vector<Tool> default_tools(bool has_rg = false, LuaHost lua_host = { });

} // namespace imza
