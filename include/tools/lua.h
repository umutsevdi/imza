#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/modal.h"
#include "common/types.h"
#include "permissions/filesystem.h"
extern "C" {
#include <lua.h>
}

namespace imza {

enum class LuaCapability { NONE, SHELL, WEB };

struct LuaMethod {
    std::string_view name;
    lua_CFunction function;
    std::string_view description;
    LuaCapability capability           = LuaCapability::NONE;
    std::string_view capability_denied = "";
    bool is_private                    = false;
};

struct LuaModule {
    bool autoload = false;
    std::string_view name;
    std::string_view description;
    std::span<const std::string_view> types;
    std::span<const LuaMethod> methods;
};

constexpr std::size_t MAX_OUTPUT_BYTES = 64 * 1024;

struct Tool; // defined in tools/tool.h; returned by value from make_lua_tool

class LuaState final : public ApplicationComponent {
public:
    LuaState();

    LuaState(const LuaState&)            = delete;
    LuaState& operator=(const LuaState&) = delete;
    ~LuaState();

    void register_module(const LuaModule& module);
    std::span<const LuaModule> modules() const;

private:
    std::vector<LuaModule> _modules;
};

std::unique_ptr<LuaState> make_lua_state();
std::string render_module_documentation(const LuaModule& module);

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
};

Tool make_lua_tool(LuaState& state, LuaHost host = { });

} // namespace imza
