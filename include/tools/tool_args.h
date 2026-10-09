#pragma once

#include <optional>
#include <string>
#include <vector>

namespace imza {

// Typed tool arguments. Each tool parses its wire args into one of these
// with the shared Glaze options; validation errors map onto the stable
// model-facing tool_error messages at the call sites.

struct SkillToolArgs {
    std::string name;
    std::optional<std::string> scope;
    // The /skill flow and re-evaluated prompts bind an exact path.
    std::optional<std::string> path;
};

struct LoadToolArgs {
    std::string name;
};

struct LoadMcpToolArgs {
    std::string server;
};

struct SubagentTaskArgs {
    std::string mode;
    std::string prompt;
};

struct SubagentToolArgs {
    std::vector<SubagentTaskArgs> tasks;
};

struct LuaToolArgs {
    std::string script;
    std::optional<std::int64_t> timeout;
};

// Stable model-facing validation messages, shared by the permission gate and
// the tool handlers so both reject with identical wording.
inline constexpr std::string_view LUA_ARGS_ERROR
    = "lua: expected a non-empty 'script' string";
inline constexpr std::string_view LOAD_ARGS_ERROR
    = "load: expected a module name";
inline constexpr std::string_view LOAD_MCP_ARGS_ERROR
    = "load_mcp: expected a server id";
inline constexpr std::string_view SUBAGENT_ARGS_ERROR
    = "subagent: expected one to five tasks";

} // namespace imza