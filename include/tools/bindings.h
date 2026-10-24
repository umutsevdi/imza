#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/modal.h"
#include "common/permission_request.h"
#include "common/tool_call.h"
#include "network/json.h"
#include "permissions/filesystem.h"
#include "tools/lua.h"

extern "C" {
#include <lua.h>
}

namespace imza {

// Per-file net mutation state for imza.fs.*: original content at first
// touch, latest content after each accepted call. The driver turns these
// into one diff per touched file when the run finishes.
struct FileMutation {
    // How the file changed, so the diff can say "deleted" or "created"
    // rather than showing an unlabeled empty side. REMOVED renders as a
    // whole-file deletion, ADDED as a whole-file addition; MODIFIED is an
    // ordinary content diff. A move records both: REMOVED at the source,
    // ADDED at the destination.
    std::string path;
    std::string original;
    std::string latest;
    FileChange::Kind kind = FileChange::Kind::MODIFIED;
    // An existing empty file and a missing one both read as "", but only
    // the missing one is a creation.
    bool existed = true;
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
    // Hard wall-clock cap no binding credits: the instruction hook enforces
    // it alongside the credited compute deadline, so a script cannot park
    // (imza.sleep) its way past the run's total budget.
    std::chrono::steady_clock::time_point hard_deadline;
    bool truncated = false;
    std::vector<LuaBindingCall> log;
    std::vector<FileMutation> mutations;
    // One entry per accepted call, in order, for the undo stack.
    std::vector<FileChange> changes;
    std::vector<CanvasView> canvases;
    // Borrowed; set by the driver before any binding can run, so
    // binding code never null-checks it.
    const LuaHost* host = nullptr;
    // Dotted catalog path of the binding executing right now, set by the
    // registration trampoline. The gate logs under it, so the dispatch-log
    // vocabulary is the model-facing one by construction.
    std::string current_binding;
    bool blocked_permission = false;
};

// The run context stored in the VM's extra space by the driver.
LuaRunContext* run_of(lua_State* L);

// Raises a Lua error carrying `message`: every binding failure aborts the
// script; wrap the call in pcall to tolerate it. Logs a failed dispatch
// entry unless the caller already logged one (gate denials do).
int binding_error(
    lua_State* L, const std::string& message, bool already_logged = false);

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

// Credits `paused` back to the run's compute deadline so a blocking binding
// (shell, sleep, ask) does not consume the script's execution budget. The
// hard deadline is never credited, so an unbounded park loop still dies.
void credit_deadline(
    LuaRunContext& run, std::chrono::steady_clock::duration paused);

// Resolves an optional workspace argument against the session working
// directory (the process cwd already tracks it). Returns 0 and fills
// `workspace` (empty when absent) on success; raises the binding error and
// returns the Lua return count on a bad directory.
int resolve_workspace(lua_State* L, const std::optional<std::string>& given,
    std::string& workspace);

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

// Value when the argument is present and not nil; type errors still raise.
std::optional<std::string> opt_string(lua_State* L, int index);
std::optional<lua_Integer> opt_integer(lua_State* L, int index);
std::optional<bool> opt_boolean(lua_State* L, int index);
// Reads an optional name -> value string table (e.g. imza.shell's env).
// Absent or nil gives nullopt; a non-table or a non-string key/value
// raises. Empty table gives an empty map (still merge-over-inherited).
std::optional<std::map<std::string, std::string>> opt_env_table(
    lua_State* L, int index);

// Converts the Lua value at `index` into JSON: scalars directly, tables
// as arrays (1-based contiguous integer keys) or objects (string keys).
// Bounded depth/size; false with `error` on unsupported values, cycles,
// or mixed-key tables.
bool lua_value_to_json(
    lua_State* L, int index, JsonValue& out, std::string& error);

// On denial raises the binding error and returns the binding return count
// (unreachable past the raise); otherwise fills `target` with the
// canonical path.
int authorize_target(lua_State* L, const FilesystemRequest& request,
    const std::string& path, std::string& target);

// One accessor per binding module (one file under src/tools/bindings/):
// the module's complete registration metadata, owned by that module's
// implementation.
const LuaModule& core_module();
const LuaModule& fs_module();
const LuaModule& web_module();
const LuaModule& canvas_module();
const LuaModule& plan_module();
const LuaModule& build_module();
const LuaModule& mcp_module();
const LuaModule& bg_module();

} // namespace imza
