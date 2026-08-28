#pragma once

#include <functional>
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

class ToolRegistry {
public:
    void add(Tool tool);
    const Tool* find(std::string_view name) const;
    const std::vector<Tool>& tools() const { return tools_; }
    std::vector<ToolSpec> specs() const;
    std::vector<ToolSpec> specs(ToolSafety safety) const;
    std::vector<ToolSpec> plan_specs() const;
    ToolOutput dispatch(const ToolCallRequest& req) const;

private:
    std::vector<Tool> tools_;
};

Tool make_read_tool();
Tool make_list_tool();
Tool make_ask_tool();
Tool make_shell_tool();
Tool make_todo_tool();
Tool make_edit_tool();
Tool make_write_tool();
ToolRegistry builtin_tools();

} // namespace ursa
