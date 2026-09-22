#include "tools/bindings.h"

#include "tools/lua.h"
#include "tools/tool.h"

#include "common/util.h"
#include "network/json_io.h"
#include "tools/file_ops.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace imza {

namespace {

    constexpr std::size_t MAX_MEMORY_BYTES         = 256UL * 1024 * 1024;
    constexpr std::size_t MAX_RETURN_DEPTH         = 16;
    constexpr std::size_t MAX_RETURN_NODES         = 10'000;
    constexpr std::size_t MAX_RETURN_TABLE_ENTRIES = 5'000;
    constexpr std::string_view TRUNCATION_MARKER   = "\n[truncated]";
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
        constexpr std::size_t payload_limit
            = MAX_OUTPUT_BYTES - TRUNCATION_MARKER.size();
        const auto append = [&](std::string_view text) {
            const std::size_t available = payload_limit - run->output.size();
            run->output.append(text.substr(0, available));
            if (text.size() > available) {
                run->truncated = true;
            }
        };
        const int n = lua_gettop(L);
        for (int i = 1; i <= n && !run->truncated; ++i) {
            if (i > 1) {
                append("    ");
            }
            std::size_t len = 0;
            const char* s   = luaL_tolstring(L, i, &len);
            append(std::string_view(s, len));
            lua_pop(L, 1);
        }
        if (!run->truncated) {
            append("\n");
        }
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
            add(tree_lua_bindings());
            return all;
        }();
        return bindings;
    }

    // True when the run's host grants `capability`.
    bool capability_allowed(const LuaHost& host, LuaCapability capability)
    {
        switch (capability) {
        case LuaCapability::SHELL: return host.shell_enabled;
        case LuaCapability::WEB: return host.web_enabled;
        case LuaCapability::NONE: return true;
        }
        return true;
    }

    // Every catalog entry is installed as this closure with the descriptor
    // as upvalue: it records the executing binding's dotted path on the run
    // context (the gate logs under it, so the dispatch-log vocabulary is the
    // model-facing one by construction), then calls the real function. When
    // the run lacks the capability it fails closed as `nil, err` with the
    // descriptor's denial text; the binding stays documented and callable.
    int binding_trampoline(lua_State* L)
    {
        const auto* binding = static_cast<const LuaBinding*>(
            lua_touserdata(L, lua_upvalueindex(1)));
        LuaRunContext* run   = run_of(L);
        run->current_binding = binding->path;
        if (!capability_allowed(*run->host, binding->capability)) {
            return binding_error(L, std::string(binding->capability_denied));
        }
        return binding->function(L);
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
            lua_pushlightuserdata(L, const_cast<LuaBinding*>(&binding));
            lua_pushcclosure(L, binding_trampoline, 1);
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
            = R"desc(Executes a sandboxed Lua script and returns printed content,
returned object, and modified files. End the script with `return expr`
whenever you have a result: any Lua expression is converted to JSON and
shown back to you. Prefer return over print -- printing a table only
shows its address; print is for progress logs.
Base libraries: string, table, math, coroutine (io/os/package are absent).

TYPES
  FileEntry  = { path: string, type: "file" | "dir", size?: string }  -- "4.2 KB"
  TodoStatus = "pending" | "in_progress" | "completed" | "cancelled"
  TodoItem   = { content: string, status: TodoStatus }
  AskCard    = { prompt: string, options?: string[], multi?: bool, free_text?: bool }
  AskAnswer  = { question: string, answer: string }
  GrepHit    = { file: string, line: integer, text: string }
  TsSymbol   = { kind: string, name: string, start_line: integer, end_line: integer, text: string }

LEGEND
  tool.<name>(args...) => Value | (nil, Err)
  - Err is a string
  - `?` optional with its default after `=`.
  - An ungranted path returns nil, Err. Check the second return value
  - Paths can be relative to current working directory.
  - Operational failures (missing file, denied permission, timeout) are
    values: nil, Err. Wrong argument types raise and abort the script;
    wrap in pcall only if you intend to survive them

METHODS)desc";
        for (const LuaBinding& binding : all_bindings()) {
            if (!binding.is_private) {
                out += "\n";
                out += binding.description;
            }
        }
        // The generated description ends at the last binding's prose: no
        // trailing newline, so the tool spec reads as a single block.
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        return out;
    }

    bool lua_value_json(lua_State* L, int index, Json::Value& out,
        std::size_t depth, std::size_t& nodes,
        std::unordered_set<const void*>& tables, std::string& error)
    {
        if (depth > MAX_RETURN_DEPTH) {
            error = "nesting exceeds " + std::to_string(MAX_RETURN_DEPTH);
            return false;
        }
        if (++nodes > MAX_RETURN_NODES) {
            error = "value exceeds " + std::to_string(MAX_RETURN_NODES)
                + " nodes";
            return false;
        }

        index = lua_absindex(L, index);
        switch (lua_type(L, index)) {
        case LUA_TNIL: out = Json::Value::null; return true;
        case LUA_TBOOLEAN: out = lua_toboolean(L, index) != 0; return true;
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) {
                out = static_cast<Json::Int64>(lua_tointeger(L, index));
                return true;
            }
            if (const lua_Number value = lua_tonumber(L, index);
                std::isfinite(value)) {
                out = static_cast<double>(value);
                return true;
            }
            error = "numbers must be finite";
            return false;
        case LUA_TSTRING: {
            std::size_t size  = 0;
            const char* value = lua_tolstring(L, index, &size);
            out               = std::string(value, size);
            return true;
        }
        case LUA_TTABLE: break;
        default:
            error = std::string("unsupported ") + luaL_typename(L, index)
                + " value";
            return false;
        }

        const void* identity = lua_topointer(L, index);
        if (!tables.insert(identity).second) {
            error = "cyclic or repeated table reference";
            return false;
        }

        enum class TableKind { EMPTY, ARRAY, OBJECT };
        TableKind kind          = TableKind::EMPTY;
        std::size_t entries     = 0;
        lua_Integer largest_key = 0;
        Json::Value value(Json::objectValue);
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (++entries > MAX_RETURN_TABLE_ENTRIES) {
                lua_pop(L, 2);
                error = "table exceeds "
                    + std::to_string(MAX_RETURN_TABLE_ENTRIES) + " entries";
                return false;
            }

            Json::Value child;
            if (!lua_value_json(
                    L, -1, child, depth + 1, nodes, tables, error)) {
                lua_pop(L, 2);
                return false;
            }
            if (lua_isinteger(L, -2)) {
                if (kind == TableKind::OBJECT) {
                    lua_pop(L, 2);
                    error = "tables cannot mix array and object keys";
                    return false;
                }
                const lua_Integer key = lua_tointeger(L, -2);
                if (key < 1
                    || key
                        > static_cast<lua_Integer>(MAX_RETURN_TABLE_ENTRIES)) {
                    lua_pop(L, 2);
                    error = "array keys must be between 1 and "
                        + std::to_string(MAX_RETURN_TABLE_ENTRIES);
                    return false;
                }
                kind = TableKind::ARRAY;
                if (!value.isArray()) {
                    value = Json::Value(Json::arrayValue);
                }
                value[static_cast<Json::ArrayIndex>(key - 1)]
                    = std::move(child);
                largest_key = std::max(largest_key, key);
            } else if (lua_type(L, -2) == LUA_TSTRING) {
                if (kind == TableKind::ARRAY) {
                    lua_pop(L, 2);
                    error = "tables cannot mix array and object keys";
                    return false;
                }
                std::size_t size              = 0;
                const char* key               = lua_tolstring(L, -2, &size);
                kind                          = TableKind::OBJECT;
                value[std::string(key, size)] = std::move(child);
            } else {
                lua_pop(L, 2);
                error = "table keys must be strings or positive integers";
                return false;
            }
            lua_pop(L, 1);
        }
        if (kind == TableKind::ARRAY
            && largest_key != static_cast<lua_Integer>(entries)) {
            error = "array keys must be contiguous from 1";
            return false;
        }
        out = std::move(value);
        return true;
    }

    std::optional<Json::Value> lua_return_value(
        lua_State* L, int first, std::string& error)
    {
        const int count = lua_gettop(L) - first + 1;
        if (count <= 0) {
            return std::nullopt;
        }

        std::size_t nodes = 0;
        std::unordered_set<const void*> tables;
        Json::Value value;
        if (count == 1) {
            if (!lua_value_json(L, first, value, 0, nodes, tables, error)) {
                return std::nullopt;
            }
        } else {
            value = Json::Value(Json::arrayValue);
            for (int i = 0; i < count; ++i) {
                Json::Value entry;
                if (!lua_value_json(
                        L, first + i, entry, 0, nodes, tables, error)) {
                    return std::nullopt;
                }
                value.append(std::move(entry));
            }
        }
        if (write_json(value).size() > MAX_OUTPUT_BYTES) {
            error = "encoded value exceeds 64 KiB";
            return std::nullopt;
        }
        return value;
    }

    ToolOutput lua_run(const Json::Value& args, const LuaHost& host)
    {
        const std::string script = json_string(args, "script");
        if (script.empty()) {
            return tool_error("lua: expected a non-empty 'script' string");
        }
        long timeout = 10;
        if (const auto value = json_int(args, "timeout")) {
            timeout = std::clamp(static_cast<long>(*value), 1L, 120L);
        }

        LuaRunContext run;
        run.deadline
            = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        run.host = &host;

        lua_State* L = lua_newstate(lua_alloc, &run);
        if (L == nullptr) {
            return tool_error("lua: cannot create VM");
        }
        open_sandbox(L, run);
        register_bindings(L);
        lua_sethook(L, deadline_hook, LUA_MASKCOUNT, HOOK_INTERVAL);

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

        const int first_result = lua_gettop(L) + 1;
        const int loaded
            = luaL_loadbufferx(L, script.data(), script.size(), "script", "t");
        if (loaded != LUA_OK) {
            return finish(
                tool_error("lua: " + std::string(lua_tostring(L, -1))));
        }
        if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
            return finish(
                tool_error("lua: " + std::string(lua_tostring(L, -1))));
        }

        std::string return_error;
        std::optional<Json::Value> return_value
            = lua_return_value(L, first_result, return_error);
        if (!return_error.empty()) {
            return finish(tool_error("lua: return value: " + return_error));
        }

        std::string output = std::move(run.output);
        if (run.truncated) {
            output += TRUNCATION_MARKER;
        }
        ToolOutput result   = tool_output(std::move(output));
        result.return_value = std::move(return_value);
        return finish(std::move(result));
    }

} // namespace

Tool make_lua_tool(LuaHost host)
{
    ToolSpec spec;
    spec.name        = "lua";
    spec.description = render_description();
    spec.parameters  = parse_json(
        R"json({"type":"object","properties":{"script":{"type":"string","description":"Lua source code to execute"},"timeout":{"type":"integer","description":"maximum script execution time in seconds, excluding pauses for permission prompts (default 10, max 120)"}},"required":["script"]})json");
    return { std::move(spec),
        [host = std::move(host)](const ToolCallRequest&,
            const Json::Value& args) { return lua_run(args, host); } };
}

} // namespace imza
