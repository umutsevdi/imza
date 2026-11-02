#include "acp/updates.h"

#include "conversation/tool_format.h"

namespace imza::acp {

using imza::parse_rpc_frame;
using imza::rpc_error;
using imza::rpc_notification;
using imza::rpc_request;
using imza::rpc_response;

namespace {

    std::string todo_status(TodoItem::Status status)
    {
        switch (status) {
        case TodoItem::Status::PENDING: return "pending";
        case TodoItem::Status::IN_PROGRESS: return "in_progress";
        case TodoItem::Status::COMPLETED: return "completed";
        case TodoItem::Status::CANCELLED: return "cancelled";
        }
        return "pending";
    }

    std::string result_status(const ToolCall& call)
    {
        if (!call.result.has_value()) {
            return call.phase == ToolCall::Phase::PLANNING ? "pending"
                                                           : "in_progress";
        }
        switch (call.result->kind) {
        case ToolCall::Result::Kind::OUTPUT:
        case ToolCall::Result::Kind::CANCEL: return "completed";
        case ToolCall::Result::Kind::ERROR:
        case ToolCall::Result::Kind::REJECT: return "failed";
        }
        return "completed";
    }

    JsonValue diff_content(const DiffView& diff)
    {
        // The ACP diff block wants whole-file texts; imza stores per-row
        // hunks, so they are re-folded: removed lines into oldText, added
        // lines into newText, unchanged lines into both. An all-add diff
        // carries null oldText (a new file).
        std::string old_text;
        std::string new_text;
        for (const DiffRow& row : diff.rows) {
            if (row.left_no.has_value()) {
                old_text += row.left;
                old_text += '\n';
            }
            if (row.right_no.has_value()) {
                new_text += row.right;
                new_text += '\n';
            }
        }
        JsonValue block;
        block["type"] = "diff";
        block["path"] = diff.file;
        block["oldText"]
            = old_text.empty() ? JsonValue { } : JsonValue(old_text);
        block["newText"] = new_text;
        return block;
    }

} // namespace

UpdateBuilder::UpdateBuilder(std::string session_id)
    : _session_id(std::move(session_id))
{
}

std::string UpdateBuilder::emit(JsonValue update)
{
    return rpc_notification(
        "session/update", session_update(_session_id, std::move(update)));
}

std::string UpdateBuilder::agent_message(
    std::string_view text, std::int64_t message_id)
{
    JsonValue update    = update_of("agent_message_chunk");
    update["content"]   = text_block(text);
    update["messageId"] = static_cast<double>(message_id);
    return emit(std::move(update));
}

std::string UpdateBuilder::agent_thought(
    std::string_view text, std::int64_t message_id)
{
    JsonValue update    = update_of("agent_thought_chunk");
    update["content"]   = text_block(text);
    update["messageId"] = static_cast<double>(message_id);
    return emit(std::move(update));
}

namespace {

    JsonValue tool_call_payload(const ToolCall& call, const char* kind)
    {
        JsonValue update = update_of(kind);
        update["toolCallId"]
            = call.call_id.empty() ? std::to_string(call.id) : call.call_id;
        update["name"] = call.name;
        update["kind"]
            = std::string(tool_kind_name(tool_kind_for(call.name, call)));
        update["status"] = result_status(call);
        // The title is what the client shows on the row; it carries the
        // UI's own label ("Executed · 2 reads · 1 command").
        update["title"] = execution_label(call);
        // rawInput/rawOutput are opaque debug fields clients render
        // verbatim, so they stay out of the display path entirely: the
        // report below is the same markdown the TUI viewer shows, with
        // the script and its output fenced and markdown output kept as
        // markdown.
        std::vector<JsonValue> content;
        if (call.result.has_value()) {
            const std::string report
                = tool_report_markdown(make_tool_report(call));
            if (!report.empty()) {
                JsonValue block;
                block["type"]    = "content";
                JsonValue inner  = text_block(report);
                block["content"] = std::move(inner);
                content.push_back(std::move(block));
            }
            for (const DiffView& diff : call.result->diffs) {
                JsonValue block;
                block["type"]    = "content";
                block["content"] = diff_content(diff);
                content.push_back(std::move(block));
            }
        }
        if (!content.empty()) {
            update["content"] = JsonValue(std::move(content));
        }
        return update;
    }

} // namespace

std::string UpdateBuilder::tool_call(const ToolCall& call)
{
    return emit(tool_call_payload(call, "tool_call"));
}

std::string UpdateBuilder::tool_call_update(const ToolCall& call)
{
    JsonValue update = tool_call_payload(call, "tool_call_update");
    // Locations are display metadata; every touched file is reported so
    // the client can follow the agent (dedup is the client's job).
    if (call.result.has_value() && !call.result->diffs.empty()) {
        JsonValue::array_t locations;
        for (const DiffView& diff : call.result->diffs) {
            locations.push_back(JsonValue(diff.file));
        }
        update["locations"] = JsonValue(std::move(locations));
    }
    return emit(std::move(update));
}

std::string UpdateBuilder::plan(const TodoList& todo)
{
    JsonValue update = update_of("plan");
    JsonValue::array_t entries;
    entries.reserve(todo.items.size());
    for (const TodoItem& item : todo.items) {
        JsonValue entry;
        entry["content"]  = item.content;
        entry["priority"] = "medium";
        entry["status"]   = todo_status(item.status);
        entries.push_back(std::move(entry));
    }
    update["entries"] = std::move(entries);
    return emit(std::move(update));
}

std::string UpdateBuilder::session_info(std::string_view title)
{
    JsonValue update = update_of("session_info_update");
    update["title"]  = std::string(title);
    return emit(std::move(update));
}

std::string UpdateBuilder::compaction_update(std::int64_t compaction_id,
    std::string_view status, std::string_view summary, std::string_view error)
{
    JsonValue update       = update_of("compaction_update");
    update["compactionId"] = static_cast<double>(compaction_id);
    update["status"]       = std::string(status);
    if (!summary.empty()) {
        update["summary"] = std::string(summary);
    }
    if (!error.empty()) {
        update["error"] = std::string(error);
    }
    return emit(std::move(update));
}

std::string UpdateBuilder::compaction_completed(
    std::int64_t compaction_id, std::string_view summary)
{
    return compaction_update(compaction_id, "completed", summary, "");
}

std::string UpdateBuilder::compaction_in_progress(std::int64_t compaction_id)
{
    return compaction_update(compaction_id, "in_progress", "", "");
}

std::string UpdateBuilder::compaction_failed(
    std::int64_t compaction_id, std::string_view error)
{
    return compaction_update(compaction_id, "failed", "", error);
}

std::string UpdateBuilder::usage_update(
    std::uint64_t used, std::uint64_t size, double cost)
{
    JsonValue update = update_of("usage_update");
    update["used"]   = static_cast<double>(used);
    update["size"]   = static_cast<double>(size);
    if (cost > 0.0) {
        JsonValue money;
        money["amount"]   = cost;
        money["currency"] = "USD";
        update["cost"]    = std::move(money);
    }
    return emit(std::move(update));
}

std::string UpdateBuilder::notice(std::string_view severity,
    std::string_view title, std::string_view description)
{
    JsonValue update   = update_of("notice");
    update["severity"] = std::string(severity);
    update["title"]    = std::string(title);
    if (!description.empty()) {
        update["description"] = std::string(description);
    }
    return emit(std::move(update));
}

std::string UpdateBuilder::available_commands(
    const std::vector<std::tuple<std::string, std::string, std::string>>&
        commands)
{
    JsonValue update = update_of("available_commands_update");
    JsonValue::array_t entries;
    entries.reserve(commands.size());
    for (const auto& [name, description, hint] : commands) {
        entries.push_back(available_command(name, description, hint));
    }
    update["availableCommands"] = std::move(entries);
    return emit(std::move(update));
}

std::string UpdateBuilder::config_option_update(
    const std::vector<ConfigOption>& options)
{
    JsonValue update        = update_of("config_option_update");
    update["configOptions"] = dump_config_options(options);
    return emit(std::move(update));
}

std::string UpdateBuilder::current_mode(std::string_view mode)
{
    JsonValue update        = update_of("current_mode_update");
    update["currentModeId"] = std::string(mode);
    return emit(std::move(update));
}

std::string execution_label(const ToolCall& call)
{
    if (call.name != "lua") {
        return std::string(tool_call_head(call));
    }
    if (!call.result.has_value()) {
        return call.phase == ToolCall::Phase::PLANNING ? "Pending"
                                                       : "Executing";
    }
    const bool failed = call.result->kind == ToolCall::Result::Kind::ERROR
        || call.result->kind == ToolCall::Result::Kind::REJECT;
    std::string label = failed ? "Execution Failed" : "Executed";
    if (!failed) {
        const std::string counts = lua_dispatch_breakdown(call);
        if (!counts.empty()) {
            label += " · " + counts;
        }
    }
    return label;
}

} // namespace imza::acp
