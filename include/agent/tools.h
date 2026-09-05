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

namespace ursa {

struct ShellExit {
    int code;
};

struct ShellTimeout {
    std::chrono::seconds duration;
};

using ShellStatus = std::variant<ShellExit, ShellTimeout>;

struct ToolOutput {
    enum class Kind { OUTPUT, ERROR };
    Kind kind;
    std::string text;
    std::optional<DiffView> diff { };
    std::optional<ShellStatus> shell_status { };
};

using ToolHandler = std::function<ToolOutput(const Json::Value& args)>;

enum class ToolSafety { READ_ONLY, MUTATING };

struct Tool {
    ToolSpec spec;
    ToolHandler run;
    ToolSafety safety      = ToolSafety::MUTATING;
    bool persistent        = true;
    bool available_in_plan = false;
};

const Tool* find_tool(std::span<const Tool> tools, std::string_view name);
std::vector<ToolSpec> tool_specs(std::span<const Tool> tools);
std::vector<ToolSpec> plan_tool_specs(std::span<const Tool> tools);
ToolOutput dispatch_tool(
    std::span<const Tool> tools, const ToolCallRequest& req);

// Where a tool call's filesystem path argument lands relative to the
// workspace root.
enum class ProjectTarget { INSIDE, OUTSIDE, INVALID };
ProjectTarget classify_project_target(
    const std::string& name, const std::string& args);

std::optional<TodoList> parse_todo_args(const Json::Value& args);
std::optional<QuestionForm> parse_ask_args(const std::string& args);
std::string todo_summary(const TodoList& todo);

Tool make_read_tool();
Tool make_skill_tool();
Tool make_list_tool();
Tool make_ask_tool();
Tool make_shell_tool();
Tool make_todo_tool();
Tool make_subagent_tool();
Tool make_edit_tool();
Tool make_write_tool();
Tool make_webfetch_tool();
Tool make_websearch_tool();
std::vector<Tool> default_tools();

} // namespace ursa
