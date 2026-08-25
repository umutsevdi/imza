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
};

using ToolHandler = std::function<ToolOutput(const Json::Value& args)>;

enum class ToolSafety { READ_ONLY, MUTATING };

struct Tool {
    ToolSpec spec;
    ToolHandler run;
    ToolSafety safety = ToolSafety::MUTATING;
    bool persistent = true;
};

class ToolRegistry {
public:
    void add(Tool tool);
    const Tool* find(std::string_view name) const;
    const std::vector<Tool>& tools() const { return tools_; }
    std::vector<ToolSpec> specs() const;
    std::vector<ToolSpec> specs(ToolSafety safety) const;
    ToolOutput dispatch(const ToolCallRequest& req) const;

private:
    std::vector<Tool> tools_;
};

Tool make_read_tool();
Tool make_list_tool();
Tool make_ask_tool();
Tool make_shell_tool();
Tool make_todo_tool();
ToolRegistry builtin_tools();

} // namespace ursa
