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
#include "common/modal.h"
#include "common/tool_call.h"
#include "common/types.h"
#include "network/web.h"
#include "permissions/filesystem.h"

namespace imza {

struct ShellInvocation {
    std::string program;
    std::optional<std::string> subcommand;
    std::vector<std::string> arguments;

    bool operator==(const ShellInvocation&) const = default;
};

struct ShellAnalysis {
    enum class Reuse { SESSION, ONCE };

    std::vector<ShellInvocation> invocations;
    Reuse reuse = Reuse::ONCE;
};

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

std::optional<std::string> validate_filesystem_tool_arguments(
    std::string_view tool, const Json::Value& arguments);
std::optional<std::string> validate_shell_tool_arguments(
    const Json::Value& arguments);
std::optional<std::string> validate_subagent_tool_arguments(
    const Json::Value& arguments, bool allow_build);
ShellAnalysis analyze_shell(std::string_view command);
bool shell_builtin_allowed(std::string_view program);
bool shell_readonly_allowed(const ShellInvocation& invocation);

Tool make_skill_tool();
Tool make_subagent_tool();

// Callbacks the lua bindings use to reach the world outside the VM.
// Empty members mean the corresponding binding is unavailable (tests,
// headless without an environment): context empty = trusted mode, ask
// empty = ASK auto-rejects, todo empty = binding returns an error.
struct LuaHost {
    std::function<PermissionContext()> context;
    std::function<std::future<ModalResult>(ModalPayload)> ask;
    std::function<TodoList()> todo;
    std::function<void(TodoList)> set_todo;
    std::function<bool(PermissionStore::Grants)> install_grants;
    bool web_enabled      = false;
    bool shell_enabled    = false;
    bool skip_permissions = false;
    bool unattended       = false;
};

Tool make_lua_tool(LuaHost host = { }, bool has_rg = false);
std::vector<Tool> default_tools(bool has_rg = false, LuaHost lua_host = { });

} // namespace imza
