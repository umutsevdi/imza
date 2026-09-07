#pragma once

#include <json/json.h>

#include <string>
#include <vector>

namespace ursa {

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
};

struct TodoList {
    std::vector<TodoItem> items;
};

} // namespace ursa
