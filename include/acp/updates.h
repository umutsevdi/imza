#pragma once

#include <string>
#include <vector>

#include "acp/protocol.h"
#include "conversation/session.h"

namespace imza::acp {

// Serializes session state into session/update frames.
class UpdateBuilder {
public:
    explicit UpdateBuilder(std::string session_id);

    std::string agent_message(std::string_view text, std::int64_t message_id);
    std::string agent_thought(std::string_view text, std::int64_t message_id);
    std::string tool_call(const ToolCall& call);
    std::string tool_call_update(const ToolCall& call);
    std::string plan(const TodoList& todo);
    std::string session_info(std::string_view title);
    std::string compaction_completed(
        std::int64_t compaction_id, std::string_view summary);
    std::string available_commands(
        const std::vector<std::pair<std::string, std::string>>& commands);
    std::string config_option_update(const std::vector<ConfigOption>& options);

private:
    std::string emit(JsonValue update);
    std::string _session_id;
};

// The label the UI renders for a lua execution card ("Executed · 2 reads");
// shared with tool_format so both interfaces label runs identically.
std::string execution_label(const ToolCall& call);

} // namespace imza::acp
