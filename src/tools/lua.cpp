#include "tools/tool.h"

#include "network/json_io.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace imza {

namespace {

    constexpr std::size_t MAX_OUTPUT_BYTES = 64 * 1024;
    constexpr std::size_t MAX_MEMORY_BYTES = 256UL * 1024 * 1024;
    // Hook fires every N VM instructions to check the wall-clock deadline;
    // short scripts pay one clock read per interval.
    constexpr int HOOK_INTERVAL = 1000 * 1000;

    struct ScriptRun {
        std::string output;
        std::size_t memory_used = 0;
        std::chrono::steady_clock::time_point deadline;
        bool truncated = false;
    };

    void* lua_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
    {
        auto* run = static_cast<ScriptRun*>(ud);
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
        auto* run
            = static_cast<ScriptRun*>(lua_touserdata(L, lua_upvalueindex(1)));
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
        auto* run = *static_cast<ScriptRun**>(lua_getextraspace(L));
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

    // Only sandbox-safe libraries: the script is a pure computation surface.
    // Text-only load via luaL_loadbufferx below; dofile/loadfile/dump are
    // stripped so the script cannot reach files or precompiled chunks.
    void open_sandbox(lua_State* L, ScriptRun& run)
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

        // load stays available but is forced to text-only mode; see
        // text_only_load above.
        lua_getglobal(L, "load");
        auto* slot = static_cast<lua_CFunction*>(
            lua_newuserdatauv(L, sizeof(lua_CFunction), 0));
        *slot = lua_tocfunction(L, -2);
        lua_rawseti(L, LUA_REGISTRYINDEX, LOAD_KEY);
        lua_pop(L, 1);
        lua_pushcfunction(L, text_only_load);
        lua_setglobal(L, "load");

        *static_cast<ScriptRun**>(lua_getextraspace(L)) = &run;
        lua_pushlightuserdata(L, &run);
        lua_pushcclosure(L, lua_print, 1);
        lua_setglobal(L, "print");
    }

    ToolOutput lua_run(const Json::Value& args)
    {
        const std::string script = json_string(args, "script");
        if (script.empty()) {
            return tool_error("lua: expected a non-empty 'script' string");
        }
        long timeout = 10;
        if (const auto value = json_int(args, "timeout")) {
            timeout = std::clamp(*value, 1L, 120L);
        }

        ScriptRun run;
        run.deadline
            = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);

        lua_State* L = lua_newstate(lua_alloc, &run);
        if (L == nullptr) {
            return tool_error("lua: cannot create VM");
        }
        open_sandbox(L, run);
        lua_sethook(L, deadline_hook, LUA_MASKCOUNT, HOOK_INTERVAL);

        const int loaded
            = luaL_loadbufferx(L, script.data(), script.size(), "script", "t");
        if (loaded != LUA_OK) {
            const std::string error
                = "lua: " + std::string(lua_tostring(L, -1));
            lua_close(L);
            return tool_error(error);
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const std::string error
                = "lua: " + std::string(lua_tostring(L, -1));
            lua_close(L);
            return tool_error(error);
        }
        lua_close(L);

        std::string output = std::move(run.output);
        if (run.truncated) {
            output += "\n[truncated]";
        }
        return { ToolOutput::Kind::OUTPUT, std::move(output) };
    }

} // namespace

Tool make_lua_tool()
{
    ToolSpec spec;
    spec.name = "lua";
    spec.description
        = "Execute a Lua 5.4 script in a sandboxed VM with no filesystem or "
          "network access. Available libraries: string, table, math, "
          "coroutine (io/os/package/loadfile/dofile are absent). "
          "print(args...) writes to the returned output; errors carry the "
          "Lua line number. Use it to compose, transform, or compute data "
          "before presenting results.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"script":{"type":"string","description":"Lua source code to execute"},"timeout":{"type":"integer","description":"maximum runtime in seconds (default 10, max 120)"}},"required":["script"]})json");
    return { std::move(spec), lua_run };
}

} // namespace imza
