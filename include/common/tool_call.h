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

enum class ToolDecision { ACCEPT, ACCEPT_ALWAYS, REJECT };

struct ToolVerdict {
    ToolDecision decision = ToolDecision::REJECT;
    std::string reason;
};

struct ToolCallRequest {
    enum class ApprovalReason { TOOL_PERMISSION, OUTSIDE_WORKSPACE };

    std::string name;
    std::string args;
    std::string description { };
    std::string id { };
    ApprovalReason approval_reason = ApprovalReason::TOOL_PERMISSION;
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
