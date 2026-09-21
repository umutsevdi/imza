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
    std::string_view description;
    LuaCapability capability = LuaCapability::NONE;
    // Error text the registration-time gate returns when `capability` is
    // off for the run; only meaningful for SHELL/WEB descriptors.
    std::string_view capability_denied = "";
    bool is_private;
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
    const LuaHost* host = nullptr;
    // Dotted catalog path of the binding executing right now, set by the
    // registration trampoline. The gate logs under it, so the dispatch-log
    // vocabulary is the model-facing one by construction.
    std::string_view current_binding;
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

// Verdict of the shared gate pipeline for one binding call: exactly one
// request arm is set when allowed; denial carries the binding-error
// text (label + gate/verdict reason) when denied.
struct GateOutcome {
    std::optional<FilesystemRequest> filesystem;
    std::optional<ShellRequest> shell;
    std::string denial;

    explicit operator bool() const { return denial.empty(); }
};

// Blocks on the host's modal route and returns the verdict, crediting the
// human think-time back to the run's wall-clock deadline.
ModalResult ask_with_deadline_credit(LuaRunContext& run, ModalPayload payload);

// The shared ASK -> grant -> re-evaluate pipeline both gates run after
// their gate returned ASK: skip_permissions accepts, unattended fails
// closed (marking the run's blocked_permission), attended routes through
// ask_with_deadline_credit, and ACCEPT_FOR_SESSION installs the session
// grants and re-runs recheck (re-evaluate the gate, confirm the same
// canonical request). Denied outcomes land in the dispatch log with
// ok=false and fill denial.
bool resolve_ask(LuaRunContext& run, const std::string& label,
    const std::string& target, PermissionStore::Grants session_grants,
    PermissionPrompt prompt, const std::function<bool()>& recheck,
    std::string& denial);

// The permission chokepoints: run the typed request through the shared
// gate and resolve_ask. The binding must execute against the returned
// canonicalized request, never the raw script-supplied path. Both log the
// call under LuaRunContext::current_binding, the executing binding's
// catalog path, so the dispatch-log vocabulary cannot diverge from the
// model-facing one.
GateOutcome authorize_filesystem(lua_State* L, FilesystemRequest request);
GateOutcome authorize_shell(lua_State* L, const ShellRequest& request);

// Binding error for a denied gate call: the historical permission-denied
// text plus the gate/verdict reason when one exists.
std::string gate_denied(
    lua_State* L, const std::string& denial, const std::string& path);

// Family catalogs, defined one file per family under src/tools/bindings/.
std::span<const LuaBinding> filesystem_lua_bindings();
std::span<const LuaBinding> session_lua_bindings();
std::span<const LuaBinding> shell_lua_bindings();
std::span<const LuaBinding> web_lua_bindings();
std::span<const LuaBinding> mutation_lua_bindings();
std::span<const LuaBinding> tree_lua_bindings();

} // namespace imza
