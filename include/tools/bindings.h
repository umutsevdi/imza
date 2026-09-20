#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/modal.h"
#include "common/tool_call.h"
#include "permissions/filesystem.h"
#include "tools/lua.h"

extern "C" {
#include <lua.h>
}

namespace imza {

// Capabilities a binding needs from the host beyond the always-available
// ones; a disabled capability makes the binding fail closed as nil, err.
enum class LuaCapability { NONE, SHELL, WEB };

// One model-facing binding: the dotted path it installs on the `tool`
// table, the C function, and the signature/prose rendered into the lua
// tool description. Registration and documentation both come from the
// catalog, so a binding cannot exist without docs or be documented
// without existing.
struct LuaBinding {
    std::string_view path;
    lua_CFunction function;
    std::string_view signature;
    std::string_view description;
    LuaCapability capability = LuaCapability::NONE;
    // Error text the registration-time gate returns when `capability` is
    // off for the run; only meaningful for SHELL/WEB descriptors.
    std::string_view capability_denied = "";
};

// Per-file net mutation state for tool.file.*: original content at first
// touch, latest content after each accepted call. The driver turns these
// into one diff per touched file when the run finishes.
struct FileMutation {
    std::string path;
    std::string original;
    std::string latest;
};

// Everything a binding needs from the run, reached via run_of(). Lives on
// the driver's stack for the duration of one tool call. The output/budget
// members belong to the driver (src/tools/lua.cpp); log/mutations/
// blocked_permission are the effect record bindings accumulate and the
// driver carries out into ToolOutput.
struct LuaRunContext {
    std::string output;
    std::size_t memory_used = 0;
    std::chrono::steady_clock::time_point deadline;
    bool truncated = false;
    std::vector<LuaBindingCall> log;
    std::vector<FileMutation> mutations;
    // Borrowed; set by the driver before any binding can run, so
    // binding code never null-checks it.
    const LuaHost* host     = nullptr;
    bool blocked_permission = false;
};

constexpr std::size_t MAX_OUTPUT_BYTES = 64 * 1024;

// ---- binding helpers shared by every family --------------------------------

// The run context stored in the VM's extra space by the driver.
LuaRunContext* run_of(lua_State* L);

// Pushes the binding error convention: nil, err.
int binding_error(lua_State* L, std::string message);

void record_call(
    lua_State* L, std::string_view binding, std::string target, bool ok);

// The permission chokepoint: takes a typed request, runs it through the
// shared filesystem gate, resolves ASK verdicts through the modal queue
// (grant install + re-evaluation on accept-for-session), and collapses
// unattended or unwired ASKs to rejection. Returns the canonicalized
// request on success; the binding must execute against that target, never
// the raw script-supplied path. Accepted calls land in the dispatch log.
std::optional<FilesystemRequest> authorize_filesystem(
    lua_State* L, FilesystemRequest request, std::string_view label = "");

// Family catalogs, defined one file per family under src/tools/bindings/.
std::span<const LuaBinding> filesystem_lua_bindings();
std::span<const LuaBinding> session_lua_bindings();
std::span<const LuaBinding> shell_lua_bindings();
std::span<const LuaBinding> web_lua_bindings();
std::span<const LuaBinding> mutation_lua_bindings();

} // namespace imza
