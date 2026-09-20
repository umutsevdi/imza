#include "tools/bindings.h"

#include "tools/lua.h"
#include "tools/tool.h"

#include "common/util.h"
#include "network/json_io.h"
#include "tools/file_ops.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace imza {

namespace {

    constexpr std::size_t MAX_MEMORY_BYTES = 256UL * 1024 * 1024;
    // Hook fires every N VM instructions to check the wall-clock deadline;
    // short scripts pay one clock read per interval.
    constexpr int HOOK_INTERVAL = 1000 * 1000;

    void* lua_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
    {
        auto* run = static_cast<LuaRunContext*>(ud);
        if (nsize == 0) {
            std::free(ptr);
            run->memory_used -= osize;
            return nullptr;
        }
        const std::size_t held = ptr == nullptr ? 0 : osize;
        if (run->memory_used - held + nsize > MAX_MEMORY_BYTES) {
            return nullptr;
        }
        void* next = std::realloc(ptr, nsize);
        if (next != nullptr) {
            run->memory_used += nsize - held;
        }
        return next;
    }

    int lua_print(lua_State* L)
    {
        auto* run = static_cast<LuaRunContext*>(
            lua_touserdata(L, lua_upvalueindex(1)));
        if (run->truncated) {
            return 0;
        }
        const int n = lua_gettop(L);
        for (int i = 1; i <= n && !run->truncated; ++i) {
            std::size_t len = 0;
            const char* s   = luaL_tolstring(L, i, &len);
            if (run->output.size() + len > MAX_OUTPUT_BYTES) {
                run->truncated = true;
                lua_pop(L, 1);
                break;
            }
            run->output.append(s, len);
            if (i < n) {
                run->output.push_back('\t');
            }
            lua_pop(L, 1);
        }
        run->output.push_back('\n');
        return 0;
    }

    void deadline_hook(lua_State* L, lua_Debug*)
    {
        auto* run = *static_cast<LuaRunContext**>(lua_getextraspace(L));
        if (std::chrono::steady_clock::now() > run->deadline) {
            luaL_error(L, "execution time limit exceeded");
        }
    }

    // Text-only load: mode is the 3rd argument; force it to "t" so a
    // precompiled binary chunk cannot smuggle in code the source sandbox
    // never saw. The original loader is fetched from the registry because
    // luaB_load is static inside the amalgamation build.
    constexpr int LOAD_KEY = 'l';

    int text_only_load(lua_State* L)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, LOAD_KEY);
        const auto* load
            = reinterpret_cast<lua_CFunction*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        const int top = lua_gettop(L);
        if (top >= 3) {
            lua_pushliteral(L, "t");
            lua_replace(L, 3);
        } else {
            while (lua_gettop(L) < 2) {
                lua_pushnil(L);
            }
            lua_pushliteral(L, "t");
        }
        return (*load)(L);
    }

    // Only sandbox-safe base libraries; tool access is via the bindings on
    // the `tool` table, each of which re-runs the filesystem permission
    // evaluation before touching the disk.
    void open_sandbox(lua_State* L, LuaRunContext& run)
    {
        static const luaL_Reg loadedlibs[] = {
            { LUA_GNAME, luaopen_base },
            { LUA_COLIBNAME, luaopen_coroutine },
            { LUA_TABLIBNAME, luaopen_table },
            { LUA_STRLIBNAME, luaopen_string },
            { LUA_MATHLIBNAME, luaopen_math },
            { nullptr, nullptr },
        };
        for (const luaL_Reg* lib = loadedlibs; lib->func != nullptr; ++lib) {
            luaL_requiref(L, lib->name, lib->func, 1);
            lua_pop(L, 1);
        }
        lua_getglobal(L, LUA_GNAME);
        lua_pushnil(L);
        lua_setfield(L, -2, "dofile");
        lua_pushnil(L);
        lua_setfield(L, -2, "loadfile");
        lua_pushnil(L);
        lua_setfield(L, -2, "print");
        lua_pop(L, 1);
        lua_getglobal(L, LUA_STRLIBNAME);
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
        lua_pop(L, 1);

        lua_getglobal(L, "load");
        auto* slot = static_cast<lua_CFunction*>(
            lua_newuserdatauv(L, sizeof(lua_CFunction), 0));
        *slot = lua_tocfunction(L, -2);
        lua_rawseti(L, LUA_REGISTRYINDEX, LOAD_KEY);
        lua_pop(L, 1);
        lua_pushcfunction(L, text_only_load);
        lua_setglobal(L, "load");

        *static_cast<LuaRunContext**>(lua_getextraspace(L)) = &run;
        lua_pushlightuserdata(L, &run);
        lua_pushcclosure(L, lua_print, 1);
        lua_setglobal(L, "print");
    }
    // Roster order: the order here is the order of METHODS entries in the
    // model-facing description.
    std::span<const LuaBinding> all_bindings()
    {
        static const std::vector<LuaBinding> bindings = [] {
            std::vector<LuaBinding> all;
            const auto add = [&](std::span<const LuaBinding> family) {
                all.insert(all.end(), family.begin(), family.end());
            };
            add(filesystem_lua_bindings());
            add(session_lua_bindings());
            add(shell_lua_bindings());
            add(web_lua_bindings());
            add(mutation_lua_bindings());
            return all;
        }();
        return bindings;
    }

    // Installs every descriptor on the global `tool` table, creating the
    // intermediate table for one-level dotted paths ("todo.get", "file.edit").
    // A duplicate path is a programming error surfaced as a VM-level failure
    // rather than a silent shadow.
    void register_bindings(lua_State* L)
    {
        lua_newtable(L);
        const int tool = lua_gettop(L);
        for (const LuaBinding& binding : all_bindings()) {
            const std::string_view path = binding.path;
            const auto dot              = path.rfind('.');
            int parent                  = tool;
            const std::string prefix(path.substr(0, dot));
            const std::string leaf(
                dot == std::string_view::npos ? path : path.substr(dot + 1));
            if (dot != std::string_view::npos) {
                lua_getfield(L, tool, prefix.c_str());
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_newtable(L);
                    lua_pushvalue(L, -1);
                    lua_setfield(L, tool, prefix.c_str());
                }
                parent = lua_gettop(L);
            }
            lua_getfield(L, parent, leaf.c_str());
            const bool duplicate = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (duplicate) {
                luaL_error(L, "binding catalog: duplicate path '%s'",
                    std::string(path).c_str());
                return;
            }
            lua_pushcfunction(L, binding.function);
            lua_setfield(L, parent, leaf.c_str());
            if (dot != std::string_view::npos) {
                lua_pop(L, 1); // the intermediate table
            }
        }
        lua_pushvalue(L, tool);
        lua_setglobal(L, "tool");
        lua_pop(L, 1);
    }

    std::string render_description()
    {
        std::string out
            = R"desc(Executes a sandboxed Lua script and returns printed content
and modified files.
Base libraries: string, table, math, coroutine (io/os/package are absent).

TYPES
  FileEntry  = { path: string, type: "file" | "dir", size?: string }  -- "4.2 KB"
  TodoStatus = "pending" | "in_progress" | "completed" | "cancelled"
  TodoItem   = { content: string, status: TodoStatus }
  AskCard    = { prompt: string, options?: string[], multi?: bool, free_text?: bool }
  AskAnswer  = { question: string, answer: string }
  GrepHit    = { file: string, line: integer, text: string }

LEGEND
  tool.<name>(args...) => Value | (nil, Err)
  - Err is a string
  - `?` optional with its default after `=`.
  - An ungranted path returns nil, Err. Check the second return value

METHODS)desc";
        for (const LuaBinding& binding : all_bindings()) {
            out += "\n";
            out += binding.signature;
            out += "\n";
            for (const std::string& line :
                split_lines(std::string(binding.description))) {
                out += "    ";
                out += line;
                out += "\n";
            }
        }
        // Match the historical literal: no trailing newline after the last
        // description line.
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        return out;
    }

    ToolOutput lua_run(
        const Json::Value& args, const LuaHost& host, bool has_rg)
    {
        const std::string script = json_string(args, "script");
        if (script.empty()) {
            return tool_error("lua: expected a non-empty 'script' string");
        }
        long timeout = 10;
        if (const auto value = json_int(args, "timeout")) {
            timeout = std::clamp(*value, 1L, 120L);
        }

        LuaRunContext run;
        run.deadline
            = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        run.host   = &host;
        run.has_rg = has_rg;

        lua_State* L = lua_newstate(lua_alloc, &run);
        if (L == nullptr) {
            return tool_error("lua: cannot create VM");
        }
        open_sandbox(L, run);
        register_bindings(L);
        lua_sethook(L, deadline_hook, LUA_MASKCOUNT, HOOK_INTERVAL);

        // The log and the net per-file diffs record what ran even when the
        // script dies mid-flight, so every exit path below carries them out.
        const auto finish = [&](ToolOutput out) {
            out.dispatch_log       = std::move(run.log);
            out.blocked_permission = run.blocked_permission;
            for (const FileMutation& m : run.mutations) {
                if (m.original == m.latest) {
                    continue;
                }
                out.diffs.push_back(make_diff_view(
                    m.path, split_lines(m.original), split_lines(m.latest)));
            }
            lua_close(L);
            return out;
        };

        const int loaded
            = luaL_loadbufferx(L, script.data(), script.size(), "script", "t");
        if (loaded != LUA_OK) {
            return finish(
                tool_error("lua: " + std::string(lua_tostring(L, -1))));
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            return finish(
                tool_error("lua: " + std::string(lua_tostring(L, -1))));
        }
        std::string output = std::move(run.output);
        if (run.truncated) {
            output += "\n[truncated]";
        }
        return finish({ ToolOutput::Kind::OUTPUT, std::move(output) });
    }

} // namespace

Tool make_lua_tool(LuaHost host, bool has_rg)
{
    ToolSpec spec;
    spec.name        = "lua";
    spec.description = render_description();
    spec.parameters  = parse_json(
        R"json({"type":"object","properties":{"script":{"type":"string","description":"Lua source code to execute"},"timeout":{"type":"integer","description":"maximum script execution time in seconds, excluding pauses for permission prompts (default 10, max 120)"}},"required":["script"]})json");
    return { std::move(spec),
        [host = std::move(host), has_rg](
            const Json::Value& args) { return lua_run(args, host, has_rg); } };
}

} // namespace imza
