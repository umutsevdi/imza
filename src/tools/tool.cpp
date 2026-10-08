#include "tools/tool.h"
#include "common/util.h"
#include "network/json.h"
#include "tools/bindings.h"
#include "tools/mcp_manager.h"
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
        return tool_error(UNKNOWN_TOOL_PREFIX + req.name);
    }
    return tool->run(req);
}

Tool make_load_mcp_tool(McpToolDocsDeps deps)
{
    ToolSpec spec;
    spec.name        = "load_mcp";
    spec.description = "Load the tool reference for one MCP server from the "
                       "<mcps> block: tool names, descriptions, and argument "
                       "schemas. Call tools with imza.mcp.call.";
    spec.parameters
        = R"json({"type":"object","properties":{"server":{"type":"string","description":"MCP server id from the <mcps> block"}},"required":["server"]})json";
    return { std::move(spec),
        [deps = std::move(deps)](const ToolCallRequest& req) -> ToolOutput {
            LoadMcpToolArgs args;
            if (json_parse_checked(req.args, args) || args.server.empty()) {
                return tool_error("load_mcp: expected a server id");
            }
            McpManager* manager = deps.mcp ? deps.mcp() : nullptr;
            if (manager == nullptr) {
                return tool_error("load_mcp: MCP is not available in this run");
            }
            std::optional<McpServerSnapshot> found;
            for (const McpServerSnapshot& server : manager->snapshot()) {
                if (server.id == args.server) {
                    found = server;
                    break;
                }
            }
            if (!found) {
                return tool_error(
                    "load_mcp: unknown server '" + args.server + "'");
            }
            if (found->state != McpServerState::CONNECTED) {
                return tool_error("load_mcp: server '" + args.server
                    + "' is not connected (state: "
                    + (found->detail.empty() ? std::string("offline")
                                             : found->detail)
                    + "); connect it with /mcp");
            }
            const auto tools = manager->tools(args.server);
            if (!tools || tools->empty()) {
                return tool_error(
                    "load_mcp: server '" + args.server + "' exposes no tools");
            }

            std::string out;
            if (found->label != found->id) {
                out += "MCP server '" + found->id + "' (" + found->label + ")";
            } else {
                out += "MCP server '" + found->id + "'";
            }
            if (!found->description.empty()) {
                out += ": " + found->description;
            }
            out += "\nCall with imza.mcp.call(\"" + found->id
                + "\", \"<tool>\", {args}).\n\n";
            for (const McpToolDefinition& tool : *tools) {
                out += "- " + tool.name;
                if (tool.title) {
                    out += " (" + *tool.title + ")";
                }
                out += "\n";
                if (tool.description) {
                    out += "  " + *tool.description + "\n";
                }
                out += "  arguments: ";
                out += tool.input_schema ? json_dump(*tool.input_schema)
                                         : std::string("none");
                out += "\n";
            }
            // A big registry server can exceed the useful context size.
            constexpr std::size_t MAX_DOCS_CHARS = 40000;
            out = truncate_with_count(std::move(out), MAX_DOCS_CHARS);
            return tool_output(std::move(out));
        } };
}

std::vector<Tool> default_tools(LuaHost lua_host, SkillToolDeps skill_deps,
    SubagentToolSlot subagent, LuaState& lua_state, McpToolDocsDeps mcp_docs)
{
    std::vector<Tool> tools;
    tools.push_back(make_skill_tool(std::move(skill_deps)));
    tools.push_back(make_load_tool(lua_state));
    tools.push_back(make_load_mcp_tool(std::move(mcp_docs)));
    tools.push_back(make_subagent_tool(std::move(subagent)));
    tools.push_back(make_lua_tool(lua_state, std::move(lua_host)));
    return tools;
}

std::optional<std::string> validate_subagent_tool_arguments(
    const SubagentToolArgs& arguments, bool allow_build)
{
    if (arguments.tasks.empty() || arguments.tasks.size() > 5) {
        return "subagent: expected one to five tasks";
    }
    for (const auto& task : arguments.tasks) {
        if (task.prompt.empty() || trim(task.prompt).empty()) {
            return "subagent: every task requires a mode and prompt";
        }
        const std::string resolved = to_lower(task.mode);
        if (resolved != "research" && resolved != "build") {
            return "subagent: mode must be research or build";
        }
        if (resolved == "build" && !allow_build) {
            return "subagent: build agents require main-agent build mode";
        }
    }
    return std::nullopt;
}

Tool make_skill_tool(SkillToolDeps deps)
{
    ToolSpec spec;
    spec.name        = "skill";
    spec.description = "Load the instructions for a discovered skill by name. "
                       "Optionally specify scope as project or global.";
    spec.parameters
        = R"json({"type":"object","properties":{"name":{"type":"string"},"scope":{"type":"string","enum":["project","global"]}},"required":["name"]})json";
    // Policy, path, and size are the gate's job; _run_tool re-evaluates
    // before dispatch, so the handler only resolves, reads, and records.
    return { std::move(spec),
        [deps = std::move(deps)](const ToolCallRequest& req) -> ToolOutput {
            SkillToolArgs args;
            if (json_parse_checked(req.args, args)) {
                return tool_error("skill: expected a name");
            }
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
    spec.parameters
        = R"json({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})json";
    return { std::move(spec), [&state](const ToolCallRequest& req) {
                LoadToolArgs args;
                if (json_parse_checked(req.args, args) || args.name.empty()) {
                    return tool_error("load: expected a module name");
                }
                const std::string name = args.name;
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
                return tool_error(
                    "load: unknown module, available: " + available);
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
    spec.parameters
        = R"json({"type":"object","properties":{"tasks":{"type":"array","minItems":1,"maxItems":5,"items":{"type":"object","properties":{"mode":{"type":"string","enum":["research","build"]},"prompt":{"type":"string","minLength":1}},"required":["mode","prompt"],"additionalProperties":false}}},"required":["tasks"],"additionalProperties":false})json";
    if (!delegate) {
        delegate = std::make_shared<SubagentToolFn>();
    }
    return { std::move(spec),
        [delegate = std::move(delegate)](
            const ToolCallRequest& req) -> ToolOutput {
            SubagentToolArgs args;
            if (json_parse_checked(req.args, args)) {
                return tool_error("subagent: expected one to five tasks");
            }
            if (!*delegate) {
                return tool_error("subagent: delegation is unavailable");
            }
            return (*delegate)(req);
        } };
}

} // namespace imza
