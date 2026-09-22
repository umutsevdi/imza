#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <vector>

#include "common/modal.h"
#include "permissions/filesystem.h"

namespace imza {

constexpr std::size_t MAX_OUTPUT_BYTES = 64 * 1024;

struct Tool; // defined in tools/tool.h; returned by value from make_lua_tool

// Callbacks the lua bindings use to reach the world outside the VM.
// Empty members mean the corresponding binding is unavailable (tests,
// headless without an environment): an empty permission_context means
// trusted mode (the gate auto-allows every request), ask empty = ASK
// auto-rejects, todo empty = binding returns an error.
struct LuaHost {
    std::function<PermissionContext()> permission_context;
    std::function<std::future<ModalResult>(ModalPayload)> ask;
    std::function<TodoList()> todo;
    std::function<void(TodoList)> set_todo;
    std::function<bool(PermissionStore::Grants)> install_grants;
    bool web_enabled      = false;
    bool shell_enabled    = false;
    bool skip_permissions = false;
    bool unattended       = false;
    bool has_rg           = false;
};

Tool make_lua_tool(LuaHost host = { });

} // namespace imza
