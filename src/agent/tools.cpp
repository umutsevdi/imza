#include "tools.h"

#include <utility>

namespace ursa {

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

const Tool* ToolRegistry::find(std::string_view name) const
{
    for (const auto& t : tools_) {
        if (t.spec.name == name) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<ToolSpec> ToolRegistry::specs() const
{
    std::vector<ToolSpec> out;
    out.reserve(tools_.size());
    for (const auto& t : tools_) {
        out.push_back(t.spec);
    }
    return out;
}

ToolOutput ToolRegistry::dispatch(const ToolCallRequest& req) const
{
    const Tool* tool = find(req.name);
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

} // namespace ursa
