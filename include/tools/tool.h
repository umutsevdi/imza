#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/tool_call.h"
#include "common/types.h"
#include "network/json.h"
#include "tools/lua.h"
#include "tools/tool_args.h"

namespace imza {

struct Skill;
class SkillStore;

// Exact prefix tests match on when the model calls an unregistered tool.
inline constexpr const char* UNKNOWN_TOOL_PREFIX = "unknown tool: ";

struct ToolOutput {
    enum class Kind { OUTPUT, ERROR };
    Kind kind;
    std::string text;
    std::optional<JsonValue> return_value = std::nullopt;
    // Net per-file diffs a lua script produced through imza.fs.*;
    // multiple mutations of one file collapse into a single before/after.
    std::vector<DiffView> diffs { };
    // Charts the canvas module rendered for the chat.
    std::vector<CanvasView> canvases { };
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

// Handlers get the request alone and parse their typed args struct from
// req.args: side-channel results (subagent chats, skill loads) must be
// matched to its call id.
using ToolHandler = std::function<ToolOutput(const ToolCallRequest&)>;

struct Tool {
    ToolSpec spec;
    ToolHandler run;
};

const Tool* find_tool(std::span<const Tool> tools, std::string_view name);
std::vector<ToolSpec> tool_specs(std::span<const Tool> tools);
ToolOutput dispatch_tool(
    std::span<const Tool> tools, const ToolCallRequest& req);

std::optional<std::string> validate_subagent_tool_arguments(
    const SubagentToolArgs& arguments, bool allow_build);

// Lazy accessors, like LuaHost: the roster is built before the
// environment and store are wired. Skill policy is gated once upstream.
struct SkillToolDeps {
    std::function<std::vector<Skill>()> catalog;
    std::function<SkillStore&()> store;
};

// Docs source for load_mcp: the accessor is empty when MCP is
// unavailable in this run. Filled in wire().
struct McpToolDocsDeps {
    std::function<McpManager*()> mcp;
};

Tool make_skill_tool(SkillToolDeps deps = { });
Tool make_load_tool(LuaState& state);
Tool make_load_mcp_tool(McpToolDocsDeps deps = { });

// Slot indirection breaks the TurnRunner/Delegation cycle: the tool
// captures it empty, wire() fills it once both exist.
using SubagentToolFn   = std::function<ToolOutput(const ToolCallRequest&)>;
using SubagentToolSlot = std::shared_ptr<SubagentToolFn>;

Tool make_subagent_tool(SubagentToolSlot delegate = { });

// Builds the model-facing roster: skill, load_mcp, subagent, and the lua
// sandbox (see tools/lua.h for the LuaHost the lua tool is wired with).
std::vector<Tool> default_tools(LuaHost lua_host, SkillToolDeps skill_deps,
    SubagentToolSlot subagent, LuaState& lua_state,
    McpToolDocsDeps mcp_docs = { });

} // namespace imza
