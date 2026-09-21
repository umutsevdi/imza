#include "tools/bindings.h"

#include "permissions/filesystem.h"
#include "tools/file_ops.h"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    namespace fs = std::filesystem;

    // The run's record for `target`, or nullptr when the file has not been
    // mutated yet this run.
    FileMutation* find_mutation(LuaRunContext* run, const std::string& target)
    {
        for (FileMutation& m : run->mutations) {
            if (m.path == target) {
                return &m;
            }
        }
        return nullptr;
    }

    // Reads the file at `target` (the run's cached latest once touched),
    // applies `transform`, persists, and records the net mutation for the
    // run's final diff. `whole_file` (tool.file.write) tolerates a missing
    // target: the original is then empty, so a fresh file diffs from blank.
    bool apply_file_mutation(lua_State* L, const std::string& target,
        const std::function<std::optional<std::string>(
            const std::string&, std::string&)>& transform,
        std::string& err, bool whole_file = false)
    {
        LuaRunContext* run     = run_of(L);
        FileMutation* mutation = find_mutation(run, target);
        std::string original;
        std::string content;
        if (mutation != nullptr) {
            original = mutation->original;
            content  = mutation->latest;
        } else {
            // load_text distinguishes a missing file (whole_file: a fresh
            // write from empty) from unreadable content (always an error).
            if (!load_text(target, content, err)) {
                if (!whole_file || !err.starts_with("no such file")) {
                    return false;
                }
                err.clear();
                content.clear();
            }
            original = content;
        }
        std::optional<std::string> next = transform(content, err);
        if (!next) {
            return false;
        }
        if (!save_text(target, *next, err)) {
            return false;
        }
        if (mutation == nullptr) {
            run->mutations.push_back({ target, original, *next });
        } else {
            mutation->latest = *next;
        }
        return true;
    }

    int binding_file_insert(lua_State* L)
    {
        const std::string path = luaL_checkstring(L, 1);
        const std::string text = luaL_checkstring(L, 2);
        lua_Integer line       = 0;
        if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
            line = luaL_checkinteger(L, 3);
            if (line < 1) {
                return binding_error(L, "file.insert: line must be 1-based");
            }
        }

        const InsertFileRequest request { path, text,
            line > 0
                ? std::optional<std::size_t>(static_cast<std::size_t>(line))
                : std::nullopt };
        const GateOutcome gate = authorize_filesystem(L, request);
        if (!gate) {
            return binding_error(L, gate_denied(L, gate.denial, path));
        }
        const std::string target = filesystem_target(*gate.filesystem).string();

        std::string err;
        const std::size_t at = static_cast<std::size_t>(line);
        if (!apply_file_mutation(
                L, target,
                [&](const std::string& content, std::string& error) {
                    return insert_text(content, text, at, error);
                },
                err)) {
            return binding_error(L, "file.insert: " + err);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    int binding_file_edit(lua_State* L)
    {
        const std::string path  = luaL_checkstring(L, 1);
        const std::string old   = luaL_checkstring(L, 2);
        const std::string fresh = luaL_checkstring(L, 3);
        lua_Integer count       = 1;
        if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) {
            count = luaL_checkinteger(L, 4);
            if (count < 0) {
                return binding_error(L, "file.edit: count must be 0 or more");
            }
        }
        if (old.empty()) {
            return binding_error(L, "file.edit: old must be non-empty");
        }

        const EditFileRequest request { path, old, fresh,
            static_cast<std::size_t>(count) };
        const GateOutcome gate = authorize_filesystem(L, request);
        if (!gate) {
            return binding_error(L, gate_denied(L, gate.denial, path));
        }
        const std::string target = filesystem_target(*gate.filesystem).string();

        std::string err;
        if (!apply_file_mutation(
                L, target,
                [&](const std::string& content, std::string& error) {
                    return replace_text(content, old, fresh,
                        static_cast<std::size_t>(count), error);
                },
                err)) {
            return binding_error(L, "file.edit: " + err);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    int binding_file_write(lua_State* L)
    {
        const std::string path = luaL_checkstring(L, 1);
        const std::string text = luaL_checkstring(L, 2);

        const WriteFileRequest request { path, text };
        const GateOutcome gate = authorize_filesystem(L, request);
        if (!gate) {
            return binding_error(L, gate_denied(L, gate.denial, path));
        }
        const std::string target = filesystem_target(*gate.filesystem).string();

        std::string err;
        if (!apply_file_mutation(
                L, target,
                [&](const std::string&, std::string&) {
                    return std::optional<std::string>(text);
                },
                err, true)) {
            return binding_error(L, "file.write: " + err);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    constexpr LuaBinding BINDINGS[] = {
        {
            "file.insert",
            binding_file_insert,
            R"desc(
tool.file.insert(path: string, text: string, line?: integer=nil) => true
Inserts text before the 1-based line, pushing it down; omit line to
append at the end. A line past the end of the file is an error.
            )desc",
        },
        {
            "file.edit",
            binding_file_edit,
            R"desc(
tool.file.edit(path: string, old: string, new: string, count?: integer=1) => true
Replaces the first count occurrences of old with new; count=0 replaces all.
Old is an exact literal match, so include enough surrounding text to be unique.
Errors if old is empty or not found.)desc",
        },
        {
            "file.write",
            binding_file_write,
            R"desc(
tool.file.write(path: string, text: string) => true
Replaces the file's entire content, creating it if absent.
Prefer insert/edit for targeted changes; this discards everything else.
            )desc",
        },
    };

} // namespace

std::span<const LuaBinding> mutation_lua_bindings() { return BINDINGS; }

} // namespace imza
