#pragma once

#include <json/json.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/tool_call.h"
#include "common/types.h"
#include "tools/lua.h"

namespace imza {

struct Skill;
class SkillStore;

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

inline ToolOutput tool_output(std::string text)
{
    return { ToolOutput::Kind::OUTPUT, std::move(text) };
}

// Handlers get the request alongside its parsed args: side-channel
// results (subagent chats, skill loads) must be matched to its call id.
using ToolHandler = std::function<ToolOutput(
    const ToolCallRequest&, const Json::Value& args)>;

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

// Lazy accessors, like LuaHost: the roster is built before the
// environment and store are wired. Skill policy is gated once upstream.
struct SkillToolDeps {
    std::function<std::vector<Skill>()> catalog;
    std::function<SkillStore&()> store;
};

Tool make_skill_tool(SkillToolDeps deps = { });

// Slot indirection breaks the TurnRunner/Delegation cycle: the tool
// captures it empty, wire() fills it once both exist.
using SubagentToolFn
    = std::function<ToolOutput(const ToolCallRequest&, const Json::Value&)>;
using SubagentToolSlot = std::shared_ptr<SubagentToolFn>;

Tool make_subagent_tool(SubagentToolSlot delegate = { });

// Builds the model-facing roster: skill, subagent, and the lua sandbox
// (see tools/lua.h for the LuaHost the lua tool is wired with).
std::vector<Tool> default_tools(LuaHost lua_host = { },
    SkillToolDeps skill_deps = { }, SubagentToolSlot subagent = { });

} // namespace imza
