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
    // Result update that also reports the files the run touched, so the
    // client can follow along.
    std::string tool_call_update_with_locations(const ToolCall& call);
    std::string plan(const TodoList& todo);
    std::string session_info(std::string_view title);
    std::string compaction_update(std::int64_t compaction_id,
        std::string_view status, std::string_view summary,
        std::string_view error);
    std::string compaction_completed(
        std::int64_t compaction_id, std::string_view summary);
    std::string compaction_in_progress(std::int64_t compaction_id);
    std::string compaction_failed(
        std::int64_t compaction_id, std::string_view error);
    // usage_update: current context window usage and cumulative cost.
    std::string usage_update(
        std::uint64_t used, std::uint64_t size, double cost);
    // Live user-facing notice, not part of session history.
    std::string notice(std::string_view severity, std::string_view title,
        std::string_view description = { });
    std::string available_commands(
        const std::vector<std::pair<std::string, std::string>>& commands);
    std::string available_commands(
        const std::vector<std::tuple<std::string, std::string, std::string>>&
            commands);
    std::string config_option_update(const std::vector<ConfigOption>& options);
    // Announced whenever the session's mode changes, whoever changed it.
    std::string current_mode(std::string_view mode);

private:
    std::string emit(JsonValue update);
    std::string _session_id;
};

// The label the UI renders for a lua execution card ("Executed · 2 reads");
// shared with tool_format so both interfaces label runs identically.
std::string execution_label(const ToolCall& call);

} // namespace imza::acp
