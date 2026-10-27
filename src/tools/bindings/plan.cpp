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

    // Shared guard of plan.create and plan.edit: reject when a build
    // stint holds the doc as its contract, then when the host context
    // lacks the mutating callback. Failures raise; 0 means proceed.
    int plan_guard(lua_State* L, const std::string& binding, bool available)
    {
        if (plan_frozen(L)) {
            return binding_error(L, binding + ": unavailable in Build mode");
        }
        if (!available) {
            return binding_error(L, binding + ": unavailable in this context");
        }
        return 0;
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
        const int guard
            = plan_guard(L, "plan.create", run->host->create_plan != nullptr);
        if (guard != 0) {
            return guard;
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
        const int guard
            = plan_guard(L, "plan.edit", run->host->edit_plan != nullptr);
        if (guard != 0) {
            return guard;
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
Returns the current plan document, or "" when none exists yet.

WHEN TO READ
- Before every edit: edit() is rejected unless the document was read
  at its current revision, so the call sequence is get() -> edit().
- At the start of a Build turn that follows a plan: the document is
  the contract the build must satisfy.
- Whenever a later turn may have written or annotated: never assume
  the document still says what you last wrote.

Empty string means no plan exists; create one rather than editing.)desc",
        },
        {
            "create",
            binding_plan_create,
            R"desc((doc: string) throws
Appends a new plan document and makes it the current plan. The
previous revision is kept in the session but superseded; the user
sees only the new one. Never mutates an existing document - a
rewrite is always a fresh create, not an edit.

DOCUMENT SHAPE
Required sections, in order, under 16 KiB (aim well under 60 lines):
# Problem     (optional) one to three lines naming what is wrong
              today; omit when there is no problem.
# Requirements  formal requirement statements, one per bullet, each
              observable and testable; never vague wants or restated
              prose of the request.
# Approach    a short summary of the intended change; not a
              step-by-step essay.
# Changes     one bullet per affected file(in this format `/path/to/file`),
              then what changes there.
# Verification  how to confirm the change: commands to run, tests
              expected to pass, behavior to check.

Validation rejects a document missing any required heading or over
the cap; the error names what is missing.

REVISIONS
Planning is collaboration. After the initial document, apply user
feedback as targeted edit() calls; when the feedback reshapes the
document more than a patch allows, write the full new revision with
create and it supersedes the old. Only the agent writes the document:
the user responds in chat or pins notes on the pane (delivered as a
revise turn, see edit), never editing the text directly.

BUILD HANDOFF
Switching to Build submits the current revision once as context; in
Build mode the document is frozen (create/edit are rejected) and is
the contract the work follows. Changing the contract means going
back to Plan mode and revising there.)desc",
        },
        {
            "edit",
            binding_plan_edit,
            R"desc((old: string, new: string, count?: integer=1) throws
Replaces occurrences of the exact text `old` with `new` in the current
plan document, in place - the revision count does not advance a
superseding revision. count=1 (default) replaces the first
occurrence; count=0 replaces all. Literal match, same semantics as
imza.fs.edit: include enough surrounding text to make `old` unique.

Throws when the document changed since your last get().
Throws when no plan exists (create first).

APPLYING PINNED NOTES
A revise turn delivers user notes as "Section > anchor" locations.
get() the document, then per note: match the anchor text exactly and
edit() it (count=1); a note whose line is marked (stale) means the
content moved - locate it in the current document by its section and
intent rather than its old line. When the notes reshape the document
more than patches allow, rewrite with create() instead. Preserve the
required skeleton and section shapes; reply with one short sentence
naming what changed.)desc",
        },
    };

} // namespace

const LuaModule& plan_module()
{
    static constexpr LuaModule MODULE { false, "plan",
        R"desc(The session plan document: creating and revising it, the required
shape, applying pinned notes, and the Build handoff. Load it before
writing a plan or revising one.)desc",
        { }, BINDINGS };
    return MODULE;
}

} // namespace imza
