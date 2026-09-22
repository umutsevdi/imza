#pragma once

#include <json/json.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "permissions/filesystem.h"
#include "permissions/shell.h"

namespace imza {

struct DiffRow {
    enum class Kind { SAME, REMOVE, ADD, SKIP };
    Kind kind = Kind::SAME;
    std::optional<std::size_t> left_no;
    std::optional<std::size_t> right_no;
    std::string left;
    std::string right;
};

struct DiffView {
    std::string file;
    std::vector<DiffRow> rows;
};

struct ShellExit {
    int code;
};

struct ShellTimeout {
    std::chrono::seconds duration;
};

using ShellStatus = std::variant<ShellExit, ShellTimeout>;

// One sandbox-binding call a lua script made: what it touched and whether
// the gate allowed it. UI-only; the model transcript never includes it.
struct LuaBindingCall {
    std::string binding;
    std::string target;
    bool ok = true;

    bool operator==(const LuaBindingCall&) const = default;
};

struct ToolSpec {
    std::string name;
    std::string description;
    Json::Value parameters;
};

struct ToolCallEntry {
    std::string id;
    std::string name;
    std::string args;
};

enum class ToolDecision { ACCEPT_ONCE, ACCEPT_FOR_SESSION, REJECT };

struct ToolVerdict {
    ToolDecision decision = ToolDecision::REJECT;
    std::string reason;
};

// The skill a prompt is about, rendered as the modal's details.
struct SkillRequest {
    std::string name;
    std::string scope;
    std::string path;

    bool operator==(const SkillRequest&) const = default;
};

// The gated call's own parameters, typed per family.
using PermissionPromptRequest = std::variant<std::monostate, FilesystemRequest,
    ShellRequest, SkillRequest>;

// Approval modal payload for a gated call. `id` is the originating
// tool-call id, or "manual-skill" for the /skill flow.
struct PermissionPrompt {
    std::string name;
    std::string description;
    std::string reason;
    std::string target;
    bool allow_for_session = false;
    std::string id;
    PermissionPromptRequest request;
};

struct ToolCallRequest {
    std::string name;
    std::string args;
    std::string description;
    std::string id;
};

struct TodoItem {
    enum class Status { PENDING, IN_PROGRESS, COMPLETED, CANCELLED };
    std::string content;
    Status status = Status::PENDING;

    bool operator==(const TodoItem&) const = default;
};

struct TodoList {
    std::vector<TodoItem> items;

    bool operator==(const TodoList&) const = default;
};

} // namespace imza
