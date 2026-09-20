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

    // Reads the file at `target` (or the run's cached latest), applies
    // `transform`, persists, and records the net mutation for the run's
    // final diff.
    bool apply_file_mutation(lua_State* L, const std::string& target,
        const std::function<std::optional<std::string>(
            const std::string&, std::string&)>& transform,
        std::string& err)
    {
        LuaRunContext* run     = run_of(L);
        FileMutation* mutation = find_mutation(run, target);
        std::string content;
        if (mutation != nullptr) {
            content = mutation->latest;
        } else if (!load_text(target, content, err)) {
            return false;
        }
        std::optional<std::string> next = transform(content, err);
        if (!next) {
            return false;
        }
        if (!save_text(target, *next, err)) {
            return false;
        }
        if (mutation == nullptr) {
            run->mutations.push_back({ target, content, *next });
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
        const std::optional<FilesystemRequest> allowed
            = authorize_filesystem(L, request, "file.insert");
        if (!allowed) {
            return binding_error(L, "file.insert: permission denied: " + path);
        }
        const std::string target = filesystem_target(*allowed).string();

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
        const std::optional<FilesystemRequest> allowed
            = authorize_filesystem(L, request, "file.edit");
        if (!allowed) {
            return binding_error(L, "file.edit: permission denied: " + path);
        }
        const std::string target = filesystem_target(*allowed).string();

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
        const std::optional<FilesystemRequest> allowed
            = authorize_filesystem(L, request, "file.write");
        if (!allowed) {
            return binding_error(L, "file.write: permission denied: " + path);
        }
        const std::string target = filesystem_target(*allowed).string();

        LuaRunContext* run = run_of(L);
        std::string err;
        FileMutation* mutation = find_mutation(run, target);
        std::string original;
        if (mutation != nullptr) {
            original = mutation->original;
        } else {
            std::error_code ec;
            if (fs::exists(fs::path(target), ec)
                && !load_text(target, original, err)) {
                record_call(L, "file.write", target, false);
                return binding_error(L, "file.write: " + err);
            }
        }
        if (!save_text(target, text, err)) {
            return binding_error(L, "file.write: " + err);
        }
        if (mutation == nullptr) {
            run->mutations.push_back({ target, original, text });
        } else {
            mutation->latest = text;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    constexpr LuaBinding BINDINGS[] = {
        {
            "file.insert",
            binding_file_insert,
            "tool.file.insert(path: string, text: string, "
            "line?: integer=nil) => true",
            "Inserts text before the 1-based line, pushing it down; omit "
            "line to\n"
            "append at the end. A line past the end of the file is an error.",
        },
        {
            "file.edit",
            binding_file_edit,
            "tool.file.edit(path: string, old: string, new: string, "
            "count?: integer=1) => true",
            "Replaces the first count occurrences of old with new; count=0 "
            "replaces\n"
            "all. old is an exact literal match, so include enough "
            "surrounding text\n"
            "to be unique. Errors if old is empty or not found.",
        },
        {
            "file.write",
            binding_file_write,
            "tool.file.write(path: string, text: string) => true",
            "Replaces the file's entire content, creating it if absent. "
            "Prefer\n"
            "insert/edit for targeted changes; this discards everything "
            "else.",
        },
    };

} // namespace

std::span<const LuaBinding> mutation_lua_bindings() { return BINDINGS; }

} // namespace imza
