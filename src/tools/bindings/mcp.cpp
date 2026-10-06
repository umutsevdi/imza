#include "tools/bindings.h"

#include "common/util.h"
#include "tools/mcp_manager.h"

#include <string>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    constexpr std::size_t MAX_MCP_CHARS = 40000;

    // Cap at a UTF-8 boundary, then mark (mirrors the web bindings).
    std::string cap_text(std::string text)
    {
        if (text.size() <= MAX_MCP_CHARS) {
            return text;
        }
        std::string out(truncate_utf8(text, MAX_MCP_CHARS));
        out += "\n[truncated: showing first " + std::to_string(out.size())
            + " of the content]";
        return out;
    }

    // All non-empty text blocks joined; MCP tools may split one answer
    // across several blocks.
    std::string joined_text(const McpToolCallResult& result)
    {
        std::string out;
        if (!result.content) {
            return out;
        }
        for (const McpContentItem& item : *result.content) {
            if (item.text && !trim(*item.text).empty()) {
                if (!out.empty()) {
                    out += '\n';
                }
                out += *item.text;
            }
        }
        return out;
    }

    int binding_mcp_call(lua_State* L)
    {
        const std::string server = luaL_checkstring(L, 1);
        const std::string tool   = luaL_checkstring(L, 2);
        const std::string target = server + "." + tool;

        JsonValue arguments;
        if (!lua_isnoneornil(L, 3)) {
            std::string error;
            if (!lua_value_to_json(L, 3, arguments, error)) {
                record_call(L, "mcp.call", target, false);
                return binding_error(L, "mcp.call: " + error);
            }
        }

        LuaRunContext* run  = run_of(L);
        McpManager* manager = run->host->mcp ? run->host->mcp() : nullptr;
        if (manager == nullptr) {
            record_call(L, "mcp.call", target, false);
            return binding_error(
                L, "mcp.call: MCP is not available in this run");
        }

        McpToolCallResult result;
        std::string detail;
        const Status st
            = manager->call(server, tool, arguments, result, detail);
        if (st != Status::OK) {
            record_call(L, "mcp.call", target, false);
            return binding_error(L, "mcp.call: " + detail);
        }
        record_call(L, "mcp.call", target, true);

        const std::string text = joined_text(result);
        if (result.is_error.value_or(false)) {
            // Tool execution errors are the model's to self-correct: the
            // server's message becomes the aborting error text.
            return binding_error(L,
                "mcp.call: tool error from '" + target + "'"
                    + (text.empty() ? std::string { } : ": " + cap_text(text)));
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "call",
            binding_mcp_call,
            R"desc((server: string, tool: string, args?: table) returns string, throws
Invokes `tool` on the MCP server `server` and returns its text content.
The <mcps> block of the system prompt lists the configured servers;
load_mcp("<id>") returns a server's tool reference with argument schemas.
The args table becomes the tool's JSON arguments.)desc",
            LuaCapability::MCP,
            "mcp.call: MCP access is disabled for this run",
        },
    };

} // namespace

std::span<const LuaMethod> mcp_lua_methods() { return BINDINGS; }

void register_mcp(LuaState& state)
{
    state.register_module({ false, "mcp",
        "Call tools on user-configured MCP servers.", { }, mcp_lua_methods() });
}

} // namespace imza
