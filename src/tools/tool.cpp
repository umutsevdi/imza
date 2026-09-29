#include "tools/tool.h"
#include "common/util.h"
#include "network/json_io.h"
#include "tools/bindings.h"
#include "tools/skills.h"

#include <cstdint>
#include <filesystem>
#include <optional>
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
    return tool->run(req, parse_json(req.args));
}

std::vector<Tool> default_tools(LuaHost lua_host, SkillToolDeps skill_deps,
    SubagentToolSlot subagent, LuaState& lua_state)
{
    std::vector<Tool> tools;
    tools.push_back(make_skill_tool(std::move(skill_deps)));
    tools.push_back(make_load_tool(lua_state));
    tools.push_back(make_subagent_tool(std::move(subagent)));
    tools.push_back(make_lua_tool(lua_state, std::move(lua_host)));
    return tools;
}

std::optional<std::string> validate_subagent_tool_arguments(
    const Json::Value& arguments, bool allow_build)
{
    if (!arguments.isObject() || !arguments["tasks"].isArray()
        || arguments["tasks"].empty() || arguments["tasks"].size() > 5) {
        return "subagent: expected one to five tasks";
    }
    // Keeps the validator in sync with the schema's additionalProperties.
    for (const std::string& member : arguments.getMemberNames()) {
        if (member != "tasks") {
            return "subagent: unknown property '" + member + "'";
        }
    }
    for (const Json::Value& value : arguments["tasks"]) {
        if (!value.isObject() || !value["mode"].isString()
            || !value["prompt"].isString()
            || trim(value["prompt"].asString()).empty()) {
            return "subagent: every task requires a mode and prompt";
        }
        for (const std::string& member : value.getMemberNames()) {
            if (member != "mode" && member != "prompt") {
                return "subagent: unknown task property '" + member + "'";
            }
        }
        const std::string mode = to_lower(value["mode"].asString());
        if (mode != "research" && mode != "build") {
            return "subagent: mode must be research or build";
        }
        if (mode == "build" && !allow_build) {
            return "subagent: build agents require main-agent build mode";
        }
    }
    return std::nullopt;
}

std::string json_string(const Json::Value& value, const char* key)
{
    return value.isObject() && value[key].isString() ? value[key].asString()
                                                     : std::string { };
}

std::optional<std::int64_t> json_int(const Json::Value& value, const char* key)
{
    if (!value.isObject() || !value[key].isIntegral()) {
        return std::nullopt;
    }
    return value[key].asInt64();
}

Tool make_skill_tool(SkillToolDeps deps)
{
    ToolSpec spec;
    spec.name        = "skill";
    spec.description = "Load the instructions for a discovered skill by name. "
                       "Optionally specify scope as project or global.";
    spec.parameters  = parse_json(
        R"json({"type":"object","properties":{"name":{"type":"string"},"scope":{"type":"string","enum":["project","global"]}},"required":["name"]})json");
    // Policy, path, and size are the gate's job; _run_tool re-evaluates
    // before dispatch, so the handler only resolves, reads, and records.
    return { std::move(spec),
        [deps = std::move(deps)](
            const ToolCallRequest&, const Json::Value& args) -> ToolOutput {
            const std::vector<Skill> catalog
                = deps.catalog ? deps.catalog() : std::vector<Skill> { };
            const std::optional<Skill> skill = resolve_skill(catalog, args);
            if (!skill) {
                return tool_error("skill: unknown or unavailable skill");
            }
            std::string reason;
            const std::optional<std::string> body
                = load_skill_checked(*skill, reason);
            if (!body) {
                return tool_error("skill: " + reason);
            }
            if (deps.store) {
                const std::optional<std::filesystem::path> path
                    = canonical_skill_path(*skill);
                deps.store().record_tool_load(
                    path.value_or(skill->path), *body);
            }
            return tool_output(*body);
        } };
}

Tool make_load_tool(LuaState& state)
{
    ToolSpec spec;
    spec.name        = "load";
    spec.description = "Load the documentation for a Lua module by name. "
                       "Returns the module's TYPES and METHODS reference; "
                       "call it before first use of any module listed under "
                       "<modules> in the lua tool description.";
    spec.parameters  = parse_json(
        R"json({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})json");
    return { std::move(spec),
        [&state](const ToolCallRequest&, const Json::Value& args) {
            const std::string name = json_string(args, "name");
            if (name.empty()) {
                return tool_error("load: expected a module name");
            }
            for (const LuaModule& module : state.modules()) {
                if (module.name == name) {
                    return tool_output(render_module_documentation(module));
                }
            }
            std::string available;
            for (const LuaModule& module : state.modules()) {
                if (!available.empty()) {
                    available += ", ";
                }
                available += module.name;
            }
            return tool_error("load: unknown module, available: " + available);
        } };
}

Tool make_subagent_tool(SubagentToolSlot delegate)
{
    ToolSpec spec;
    spec.name = "subagent";
    spec.description
        = "Delegate one to five independent tasks to concurrent research or "
          "build agents and wait for their reports. Build agents are only "
          "available while the main agent is in build mode.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"tasks":{"type":"array","minItems":1,"maxItems":5,"items":{"type":"object","properties":{"mode":{"type":"string","enum":["research","build"]},"prompt":{"type":"string","minLength":1}},"required":["mode","prompt"],"additionalProperties":false}}},"required":["tasks"],"additionalProperties":false})json");
    if (!delegate) {
        delegate = std::make_shared<SubagentToolFn>();
    }
    return { std::move(spec),
        [delegate = std::move(delegate)](
            const ToolCallRequest& req, const Json::Value& args) -> ToolOutput {
            if (!*delegate) {
                return tool_error("subagent: delegation is unavailable");
            }
            return (*delegate)(req, args);
        } };
}

} // namespace imza
