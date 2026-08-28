#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "network.h"
#include "types.h"

namespace ursa {

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
    ToolSafety safety = ToolSafety::MUTATING;
    bool persistent = true;
    bool available_in_plan = false;
};

const Tool* find_tool(
    std::span<const Tool> tools, std::string_view name);
std::vector<ToolSpec> tool_specs(std::span<const Tool> tools);
std::vector<ToolSpec> plan_tool_specs(std::span<const Tool> tools);
ToolOutput dispatch_tool(
    std::span<const Tool> tools, const ToolCallRequest& req);

Tool make_read_tool();
Tool make_list_tool();
Tool make_ask_tool();
Tool make_shell_tool();
Tool make_todo_tool();
Tool make_edit_tool();
Tool make_write_tool();
std::vector<Tool> default_tools();

} // namespace ursa
