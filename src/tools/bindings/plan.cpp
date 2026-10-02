#include "tools/bindings.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    // Session-state mutation, like imza.todo.*: not gated by the
    // permission layer, but frozen while a build stint holds the doc
    // as its contract.
    bool plan_frozen(lua_State* L)
    {
        LuaRunContext* run = run_of(L);
        return run->host->plan_frozen && run->host->plan_frozen();
    }

    int binding_plan_get(lua_State* L)
    {
        LuaRunContext* run = run_of(L);
        if (!run->host->plan_doc) {
            return binding_error(L, "plan.get: unavailable in this context");
        }
        const std::string content = run->host->plan_doc();
        run->host->mark_plan_seen();
        record_call(L, "plan.get", "", true);
        lua_pushlstring(L, content.data(), content.size());
        return 1;
    }

    int binding_plan_create(lua_State* L)
    {
        const std::string doc = luaL_checkstring(L, 1);
        LuaRunContext* run    = run_of(L);
        if (plan_frozen(L)) {
            return binding_error(L, "plan.create: unavailable in Build mode");
        }
        if (!run->host->create_plan) {
            return binding_error(L, "plan.create: unavailable in this context");
        }
        if (const std::string error = run->host->create_plan(doc);
            !error.empty()) {
            record_call(L, "plan.create", "", false);
            return binding_error(L, "plan.create: " + error);
        }
        record_call(L, "plan.create", "", true);
        return 0;
    }

    int binding_plan_edit(lua_State* L)
    {
        const std::string old   = luaL_checkstring(L, 1);
        const std::string fresh = luaL_checkstring(L, 2);
        std::size_t count       = 1;
        if (const auto given = opt_integer(L, 3)) {
            if (*given < 0) {
                return binding_error(L, "plan.edit: count must be 0 or more");
            }
            count = static_cast<std::size_t>(*given);
        }
        if (old.empty()) {
            return binding_error(L, "plan.edit: old must be non-empty");
        }
        LuaRunContext* run = run_of(L);
        if (plan_frozen(L)) {
            return binding_error(L, "plan.edit: unavailable in Build mode");
        }
        if (!run->host->edit_plan) {
            return binding_error(L, "plan.edit: unavailable in this context");
        }
        if (const std::string error = run->host->edit_plan(old, fresh, count);
            !error.empty()) {
            record_call(L, "plan.edit", "", false);
            return binding_error(L, error);
        }
        record_call(L, "plan.edit", "", true);
        return 0;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "get",
            binding_plan_get,
            R"desc(() returns string, throws
Returns the current plan document: markdown with a required skeleton
(Goal / Approach / Files / Verification / Open Questions), or an empty
string when none exists. Marks the plan as read; imza.plan.edit compares
against this.)desc",
        },
        {
            "create",
            binding_plan_create,
            R"desc((doc: string) throws
Creates a new plan document as the current plan; the previous one is
kept in the session but superseded. Validates the skeleton and the
16 KiB cap. Never mutates an existing document.)desc",
        },
        {
            "edit",
            binding_plan_edit,
            R"desc((old: string, new: string, count?: integer=1) throws
Replaces the first count occurrences of old in the current plan with new;
count=0 replaces all. Exact literal match, same semantics as imza.fs.edit.
Throws when the plan changed since the last imza.plan.get(); re-read
and retry. Throws when no plan exists.)desc",
        },
    };

} // namespace

std::span<const LuaMethod> plan_lua_methods() { return BINDINGS; }

void register_plan(LuaState& state)
{
    state.register_module({ true, "plan",
        R"desc(The session plan document: the intent object a plan-mode turn
produces. Create it with imza.plan.create when the turn concludes that
files should change; patch it with imza.plan.edit.)desc",
        { }, plan_lua_methods() });
}

} // namespace imza
