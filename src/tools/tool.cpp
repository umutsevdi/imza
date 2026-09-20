#include "tools/tool.h"
#include "common/util.h"
#include "network/json_io.h"

#include <string>
#include <utility>
#include <vector>

namespace imza {

const Tool* find_tool(std::span<const Tool> tools, std::string_view name)
{
    for (const auto& t : tools) {
        if (t.spec.name == name) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<ToolSpec> tool_specs(std::span<const Tool> tools)
{
    std::vector<ToolSpec> out;
    out.reserve(tools.size());
    for (const auto& t : tools) {
        out.push_back(t.spec);
    }
    return out;
}

ToolOutput dispatch_tool(
    std::span<const Tool> tools, const ToolCallRequest& req)
{
    const Tool* tool = find_tool(tools, req.name);
    if (tool == nullptr) {
        return { ToolOutput::Kind::ERROR, "unknown tool: " + req.name };
    }
    Json::Value args = parse_json(req.args);
    if (args.isNull()) {
        args = Json::Value(req.args);
    }
    if (!tool->run) {
        return { ToolOutput::Kind::ERROR,
            "tool has no implementation: " + req.name };
    }
    return tool->run(args);
}

std::vector<Tool> default_tools(
    RuntimeFlag flags, bool has_rg, LuaHost lua_host)
{
    std::vector<Tool> tools;
    tools.push_back(make_skill_tool());
    tools.push_back(make_subagent_tool());
    tools.push_back(make_lua_tool(std::move(lua_host), has_rg));
    return tools;
}

Tool make_skill_tool()
{
    ToolSpec spec;
    spec.name        = "skill";
    spec.description = "Load the instructions for a discovered skill by name. "
                       "Optionally specify scope as project or global.";
    spec.parameters  = parse_json(
        R"json({"type":"object","properties":{"name":{"type":"string"},"scope":{"type":"string","enum":["project","global"]}},"required":["name"]})json");
    return { std::move(spec), { } };
}

Tool make_subagent_tool()
{
    ToolSpec spec;
    spec.name = "subagent";
    spec.description
        = "Delegate one to five independent tasks to concurrent research or "
          "build agents and wait for their reports. Build agents are only "
          "available while the main agent is in build mode.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"tasks":{"type":"array","minItems":1,"maxItems":5,"items":{"type":"object","properties":{"mode":{"type":"string","enum":["research","build"]},"prompt":{"type":"string","minLength":1}},"required":["mode","prompt"],"additionalProperties":false}}},"required":["tasks"],"additionalProperties":false})json");
    return Tool { std::move(spec), { } };
}

} // namespace imza
