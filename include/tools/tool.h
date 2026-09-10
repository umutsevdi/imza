#pragma once

#include <json/json.h>

#include <chrono>
#include <functional>
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

namespace imza {

struct ShellExit {
    int code;
};

struct ShellTimeout {
    std::chrono::seconds duration;
};

using ShellStatus = std::variant<ShellExit, ShellTimeout>;

struct ShellInvocation {
    std::string program;
    std::optional<std::string> subcommand;

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
    std::optional<DiffView> diff { };
    std::optional<ShellStatus> shell_status { };
    std::optional<ViewerModal> viewer { };
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

std::optional<TodoList> parse_todo_args(const Json::Value& args);
std::optional<QuestionForm> parse_ask_args(const std::string& args);
std::optional<std::string> validate_filesystem_tool_arguments(
    std::string_view tool, const Json::Value& arguments);
std::optional<std::string> validate_shell_tool_arguments(
    const Json::Value& arguments);
std::optional<std::string> validate_web_tool_arguments(
    std::string_view tool, const Json::Value& arguments);
std::optional<std::string> validate_subagent_tool_arguments(
    const Json::Value& arguments, bool allow_build);
std::string todo_summary(const TodoList& todo);
ShellAnalysis analyze_shell(std::string_view command);
bool shell_builtin_allowed(std::string_view program);

Tool make_read_tool();
Tool make_skill_tool();
Tool make_list_tool();
Tool make_find_tool(bool has_rg);
Tool make_ask_tool();
Tool make_shell_tool();
Tool make_todo_tool();
Tool make_subagent_tool();
Tool make_edit_tool();
Tool make_write_tool();
Tool make_webfetch_tool();
Tool make_websearch_tool();
std::vector<Tool> default_tools(
    RuntimeFlag flags = interactive_runtime_flags(), bool has_rg = false);

} // namespace imza
