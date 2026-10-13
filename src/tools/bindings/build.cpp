#include "tools/bindings.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    // Session-state mutation, like imza.todo.* and imza.plan.*: not gated
    // by the permission layer. Available only in Build mode, where the
    // agent is implementing and makes judgment calls worth recording.
    int binding_build_decision(lua_State* L)
    {
        const std::string entry = luaL_checkstring(L, 1);
        LuaRunContext* run      = run_of(L);
        if (!run->host->append_build_decision) {
            return binding_error(
                L, "build.decision: unavailable in this context");
        }
        if (!run->host->build_mode || !run->host->build_mode()) {
            return binding_error(
                L, "build.decision: only available in Build mode");
        }
        if (const std::string error = run->host->append_build_decision(entry);
            !error.empty()) {
            record_call(L, "build.decision", "", false);
            return binding_error(L, "build.decision: " + error);
        }
        record_call(L, "build.decision", "", true);
        return 0;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "decision",
            binding_build_decision,
            R"desc((text: string) throws
Records one implementation decision in the session's build log, a
markdown document the user can review after the build. Call it when you
choose one approach over another on the user's behalf (an algorithm, a
dependency, a trade-off) so the reasoning is not lost. Entries append to
the current build log; a new log starts after the next plan revision.
Only available in Build mode.)desc",
        },
    };

} // namespace

const LuaModule& build_module()
{
    static constexpr LuaModule MODULE { true, "build",
        R"desc(The build log: a markdown record of the decisions you make
while implementing in Build mode. Append one with imza.build.decision;
the user reads it during review. Each build stint gets its own log.)desc",
        { }, BINDINGS };
    return MODULE;
}

} // namespace imza
