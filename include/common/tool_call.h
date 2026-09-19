#pragma once

#include <json/json.h>

#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/diff.h"

namespace imza {

struct ShellExit {
    int code;
};

struct ShellTimeout {
    std::chrono::seconds duration;
};

using ShellStatus = std::variant<ShellExit, ShellTimeout>;

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

struct ToolCallRequest {
    std::string name;
    std::string args;
    std::string description;
    std::string id;
    std::string permission_reason;
    bool allow_for_session = false;
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
