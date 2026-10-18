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

// Foreground script and shell timeout ceiling: the caller-selectable cap.
inline constexpr long MAX_SCRIPT_SECONDS = 240;
// One 10-minute ceiling for everything that may outlive a single call: the
// hard wall-clock cap on a script run, the longest background command, and the
// longest single imza.sleep. Blocking bindings (shell, sleep, ask) credit the
// compute budget back but never this, so a park loop stays bounded.
inline constexpr long MAX_TASK_SECONDS = 600;
// Background command timeout default.
inline constexpr long DEFAULT_BG_SECONDS = 120;

enum class LuaCapability { NONE, SHELL, WEB, MCP };

struct LuaMethod {
    std::string_view name;
    lua_CFunction function;
    std::string_view description;
    LuaCapability capability           = LuaCapability::NONE;
    std::string_view capability_denied = "";
};

struct LuaModule {
    bool autoload = false;
    std::string_view name;
    std::string_view description;
    std::span<const std::string_view> types;
    std::span<const LuaMethod> methods;
};

struct Tool; // defined in tools/tool.h; returned by value from make_lua_tool
class McpSubsystem;

// Immutable module registry: built once by make_lua_state, read by
// every lua tool call. A value type, not an Subsystem - it
// owns no state that outlives a call and needs no synchronization.
class LuaState {
public:
    LuaState()                           = default;
    LuaState(const LuaState&)            = delete;
    LuaState& operator=(const LuaState&) = delete;
    ~LuaState()                          = default;

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
    std::function<std::string()> plan_doc;
    std::function<std::string(std::string)> create_plan;
    std::function<std::string(
        const std::string&, const std::string&, std::size_t)>
        edit_plan;
    std::function<void()> mark_plan_seen;
    std::function<bool()> plan_frozen;
    // Appends one markdown entry to the session's current build log;
    // empty means the binding is unavailable in this context.
    std::function<std::string(std::string)> append_build_decision;
    // True when the session is in Build mode; gates imza.build.decision.
    std::function<bool()> build_mode;
    std::function<bool(PermissionStore::Grants)> install_grants;
    // MCP server sessions; empty accessor means the bindings are
    // unavailable (no manager in this run).
    std::function<McpSubsystem*()> mcp;
    // True when the run's session has an interrupt pending; imza.sleep
    // bails out on it. Empty means never interrupted (tests).
    std::function<bool()> interrupted;
    // Starts a detached background command (stdio to /dev/null, killed on
    // timeout or application teardown). Returns false when it could not be
    // spawned; empty means imza.bg.shell is unavailable in this run.
    std::function<bool(
        const std::string&, std::chrono::seconds, const std::string&)>
        spawn_bg;
    bool web_enabled   = false;
    bool shell_enabled = false;
    bool mcp_enabled   = false;
    // Live approval-skipping switch; read at each gated call so a
    // runtime /auto toggle reaches the sandbox without a restart. Empty
    // means false (tests).
    std::function<bool()> skip_permissions;
    bool unattended = false;
};

Tool make_lua_tool(LuaState& state, LuaHost host = { });

} // namespace imza
