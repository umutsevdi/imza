#pragma once

#include <functional>
#include <future>
#include <vector>

#include "common/modal.h"
#include "permissions/filesystem.h"

namespace imza {

struct Tool; // defined in tools/tool.h; returned by value from make_lua_tool

// Callbacks the lua bindings use to reach the world outside the VM.
// Empty members mean the corresponding binding is unavailable (tests,
// headless without an environment): context empty = trusted mode, ask
// empty = ASK auto-rejects, todo empty = binding returns an error.
struct LuaHost {
    std::function<PermissionContext()> context;
    std::function<std::future<ModalResult>(ModalPayload)> ask;
    std::function<TodoList()> todo;
    std::function<void(TodoList)> set_todo;
    std::function<bool(PermissionStore::Grants)> install_grants;
    bool web_enabled      = false;
    bool shell_enabled    = false;
    bool skip_permissions = false;
    bool unattended       = false;
};

Tool make_lua_tool(LuaHost host = { }, bool has_rg = false);

} // namespace imza
