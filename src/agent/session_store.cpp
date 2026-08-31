#include "session_store.h"

#include "environment.h"
#include "io.h"
#include "network.h"
#include "session.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace ursa {

namespace {

    std::string session_filename()
    {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto milliseconds
            = std::chrono::duration_cast<std::chrono::milliseconds>(now)
                  .count();
        std::random_device random;
        const std::uint32_t suffix = static_cast<std::uint32_t>(random());
        std::ostringstream out;
        out << milliseconds << '-' << std::hex << std::setw(8)
            << std::setfill('0') << suffix << ".json";
        return out.str();
    }

    Json::Value todo_json(const TodoList& todo)
    {
        Json::Value out(Json::arrayValue);
        for (const auto& item : todo.items) {
            Json::Value value;
            value["content"] = item.content;
            value["status"]  = static_cast<int>(item.status);
            out.append(std::move(value));
        }
        return out;
    }

    TodoList parse_todo(const Json::Value& value)
    {
        TodoList out;
        if (!value.isArray()) {
            return out;
        }
        for (const auto& entry : value) {
            if (!entry["content"].isString()) {
                continue;
            }
            TodoItem item;
            item.content     = entry["content"].asString();
            const int status = entry.get("status", 0).asInt();
            if (status >= 0 && status <= 3) {
                item.status = static_cast<TodoItem::Status>(status);
            }
            out.items.push_back(std::move(item));
        }
        return out;
    }

    Json::Value item_json(const ConversationItem& item)
    {
        Json::Value out;
        if (const auto* user = std::get_if<UserTurn>(&item)) {
            out["type"] = "user";
            out["text"] = user->text;
            Json::Value attachments(Json::arrayValue);
            for (const auto& attachment : user->attachments) {
                Json::Value value;
                value["path"]    = attachment.path;
                value["content"] = attachment.content;
                attachments.append(std::move(value));
            }
            out["attachments"] = std::move(attachments);
        } else if (const auto* assistant = std::get_if<AssistantTurn>(&item)) {
            out["type"]                = "assistant";
            out["markdown"]            = assistant->markdown;
            out["reasoning"]           = assistant->reasoning;
            out["reasoning_signature"] = assistant->reasoning_signature;
            out["model"]               = assistant->model;
            out["reasoning_effort"]    = assistant->reasoning_effort;
            if (assistant->reasoning_ms) {
                out["reasoning_ms"] = static_cast<Json::Int64>(
                    assistant->reasoning_ms->count());
            }
        } else if (const auto* tool = std::get_if<ToolCall>(&item)) {
            out["type"]    = "tool";
            out["id"]      = static_cast<Json::UInt64>(tool->id);
            out["call_id"] = tool->call_id;
            out["name"]    = tool->name;
            out["args"]    = tool->args;
            if (!tool->subagent_chats.empty()) {
                Json::Value chats(Json::arrayValue);
                for (const SubagentChat& chat : tool->subagent_chats) {
                    Json::Value value;
                    value["title"]      = chat.title;
                    value["transcript"] = chat.transcript;
                    chats.append(std::move(value));
                }
                out["subagent_chats"] = std::move(chats);
            }
            if (tool->result) {
                out["result_kind"] = static_cast<int>(tool->result->kind);
                out["result"]      = tool->result->text;
                if (tool->result->diff) {
                    Json::Value diff;
                    diff["file"] = tool->result->diff->file;
                    Json::Value rows(Json::arrayValue);
                    for (const auto& row : tool->result->diff->rows) {
                        Json::Value value;
                        value["kind"]  = static_cast<int>(row.kind);
                        value["left"]  = row.left;
                        value["right"] = row.right;
                        if (row.left_no) {
                            value["left_no"]
                                = static_cast<Json::UInt64>(*row.left_no);
                        }
                        if (row.right_no) {
                            value["right_no"]
                                = static_cast<Json::UInt64>(*row.right_no);
                        }
                        rows.append(std::move(value));
                    }
                    diff["rows"] = std::move(rows);
                    out["diff"]  = std::move(diff);
                }
                if (tool->result->shell_status) {
                    std::visit(
                        [&](const auto& status) {
                            using T = std::decay_t<decltype(status)>;
                            if constexpr (std::is_same_v<T, ShellExit>) {
                                out["shell_exit"] = status.code;
                            } else {
                                out["shell_timeout"] = static_cast<Json::Int64>(
                                    status.duration.count());
                            }
                        },
                        *tool->result->shell_status);
                }
            }
        } else if (const auto* todo = std::get_if<TodoList>(&item)) {
            out["type"]  = "todo";
            out["items"] = todo_json(*todo);
        } else if (const auto* event = std::get_if<CompactionEvent>(&item)) {
            out["type"]   = "compaction";
            out["id"]     = static_cast<Json::UInt64>(event->id);
            out["status"] = static_cast<int>(event->status);
        } else if (const auto* answer = std::get_if<ModalAnswer>(&item)) {
            out["type"] = "modal_answer";
            Json::Value cards(Json::arrayValue);
            for (const auto& card : answer->cards) {
                Json::Value value;
                value["prompt"]    = card.prompt;
                value["free_text"] = card.free_text;
                Json::Value selected(Json::arrayValue);
                for (const auto& choice : card.selected) {
                    selected.append(choice);
                }
                value["selected"] = std::move(selected);
                cards.append(std::move(value));
            }
            out["cards"] = std::move(cards);
        }
        return out;
    }

    std::optional<ConversationItem> parse_item(const Json::Value& value)
    {
        const std::string type = value.get("type", "").asString();
        if (type == "user") {
            UserTurn user;
            user.text = value.get("text", "").asString();
            for (const auto& entry : value["attachments"]) {
                user.attachments.push_back({ entry.get("path", "").asString(),
                    entry.get("content", "").asString() });
            }
            return user;
        }
        if (type == "assistant") {
            AssistantTurn assistant;
            assistant.markdown  = value.get("markdown", "").asString();
            assistant.reasoning = value.get("reasoning", "").asString();
            assistant.reasoning_signature
                = value.get("reasoning_signature", "").asString();
            assistant.model = value.get("model", "").asString();
            assistant.reasoning_effort
                = value.get("reasoning_effort", "").asString();
            if (value["reasoning_ms"].isInt64()) {
                assistant.reasoning_ms = std::chrono::milliseconds(
                    value["reasoning_ms"].asInt64());
            }
            return assistant;
        }
        if (type == "tool") {
            ToolCall tool;
            tool.id      = value.get("id", 0).asUInt64();
            tool.call_id = value.get("call_id", "").asString();
            tool.name    = value.get("name", "").asString();
            tool.args    = value.get("args", "").asString();
            for (const Json::Value& chat : value["subagent_chats"]) {
                if (!chat.isObject())
                    continue;
                tool.subagent_chats.push_back(
                    SubagentChat { chat.get("title", "Agent").asString(),
                        chat.get("transcript", "").asString() });
            }
            if (value.isMember("result_kind")) {
                const int kind = value["result_kind"].asInt();
                if (kind >= 0 && kind <= 3) {
                    tool.result = ToolCall::Result {
                        static_cast<ToolCall::Result::Kind>(kind),
                        value.get("result", "").asString()
                    };
                    if (value["diff"].isObject()) {
                        DiffView diff;
                        diff.file = value["diff"].get("file", "").asString();
                        for (const auto& row_value : value["diff"]["rows"]) {
                            DiffRow row;
                            const int row_kind
                                = row_value.get("kind", 0).asInt();
                            if (row_kind >= 0 && row_kind <= 2) {
                                row.kind = static_cast<DiffRow::Kind>(row_kind);
                            }
                            row.left  = row_value.get("left", "").asString();
                            row.right = row_value.get("right", "").asString();
                            if (row_value.isMember("left_no")) {
                                row.left_no = row_value["left_no"].asUInt64();
                            }
                            if (row_value.isMember("right_no")) {
                                row.right_no = row_value["right_no"].asUInt64();
                            }
                            diff.rows.push_back(std::move(row));
                        }
                        tool.result->diff = std::move(diff);
                    }
                    if (value.isMember("shell_exit")) {
                        tool.result->shell_status
                            = ShellExit { value["shell_exit"].asInt() };
                    } else if (value.isMember("shell_timeout")) {
                        tool.result->shell_status
                            = ShellTimeout { std::chrono::seconds(
                                value["shell_timeout"].asInt64()) };
                    }
                }
            }
            return tool;
        }
        if (type == "todo") {
            return parse_todo(value["items"]);
        }
        if (type == "compaction") {
            CompactionEvent event;
            event.id         = value.get("id", 0).asUInt64();
            const int status = value.get("status", 1).asInt();
            event.status     = status >= 0 && status <= 2
                ? static_cast<CompactionEvent::Status>(status)
                : CompactionEvent::Status::COMPLETED;
            return event;
        }
        if (type == "modal_answer") {
            ModalAnswer answer;
            for (const auto& card_value : value["cards"]) {
                QuestionAnswer card;
                card.prompt    = card_value.get("prompt", "").asString();
                card.free_text = card_value.get("free_text", "").asString();
                for (const auto& choice : card_value["selected"]) {
                    if (choice.isString()) {
                        card.selected.push_back(choice.asString());
                    }
                }
                answer.cards.push_back(std::move(card));
            }
            return answer;
        }
        return std::nullopt;
    }

} // namespace

std::filesystem::path data_dir()
{
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    return std::filesystem::path(appdata && *appdata ? appdata : ".") / "ursa";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : ".") / "Library"
        / "Application Support" / "ursa";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return std::filesystem::path(xdg) / "ursa";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : ".") / ".local"
        / "share" / "ursa";
#endif
}

std::filesystem::path sessions_dir() { return data_dir() / "sessions"; }

Status save_session(Session& session)
{
    const SessionSnapshot snapshot = session.snapshot();
    if (snapshot.items.empty()) {
        return Status::OK;
    }
    if (std::holds_alternative<PersistedSession>(snapshot.persistence)) {
        return Status::OK;
    }
    Json::Value root;
    std::error_code workspace_ec;
    const std::filesystem::path workspace
        = std::filesystem::current_path(workspace_ec);
    if (workspace_ec) {
        return Status::CONFIG_ERROR;
    }
    root["version"]           = 1;
    root["title"]             = snapshot.title;
    root["saved_at"]          = format_local_time("%Y-%m-%d %H:%M:%S");
    root["todo"]              = todo_json(snapshot.todo);
    root["compacted_summary"] = snapshot.compacted_summary;
    root["compacted_item_count"]
        = static_cast<Json::UInt64>(snapshot.compacted_item_count);
    root["mode"]      = snapshot.plan_mode ? "plan" : "build";
    root["workspace"] = workspace.string();
    Json::Value items(Json::arrayValue);
    for (const auto& item : snapshot.items) {
        items.append(item_json(item));
    }
    root["items"] = std::move(items);

    std::error_code ec;
    std::filesystem::path path;
    do {
        path = sessions_dir() / session_filename();
    } while (std::filesystem::exists(path, ec) && !ec);
    if (ec) {
        return Status::CONFIG_ERROR;
    }

    const Status st = write_json_file(path, root, "");
    if (st != Status::OK) {
        return st;
    }
    session.set_persistence(PersistedSession { path });
    return Status::OK;
}

Status load_session(const std::filesystem::path& path, Session& session)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Status::CONFIG_ERROR;
    }
    std::stringstream text;
    text << file.rdbuf();
    const Json::Value root = parse_json(text.str());
    if (!root.isObject() || !root["items"].isArray()) {
        return Status::JSON_ERROR;
    }
    if (!root["workspace"].isString() || root["workspace"].asString().empty()) {
        return Status::CONFIG_ERROR;
    }
    const std::filesystem::path workspace = root["workspace"].asString();
    std::error_code workspace_ec;
    if (!std::filesystem::is_directory(workspace, workspace_ec)
        || workspace_ec) {
        return Status::CONFIG_ERROR;
    }
    SessionSnapshot snapshot;
    snapshot.persistence       = PersistedSession { path };
    snapshot.title             = root.get("title", "").asString();
    snapshot.todo              = parse_todo(root["todo"]);
    snapshot.compacted_summary = root.get("compacted_summary", "").asString();
    snapshot.compacted_item_count
        = root.get("compacted_item_count", 0).asUInt64();
    snapshot.plan_mode = root.get("mode", "plan").asString() != "build";
    for (const auto& value : root["items"]) {
        if (auto item = parse_item(value)) {
            snapshot.items.push_back(std::move(*item));
        }
    }
    if (!get_environment()->chdir(workspace)) {
        return Status::CONFIG_ERROR;
    }
    session.restore(std::move(snapshot));
    return Status::OK;
}

std::vector<SavedSession> saved_sessions()
{
    std::vector<SavedSession> out;
    std::error_code ec;
    if (!std::filesystem::exists(sessions_dir(), ec)) {
        return out;
    }
    for (const auto& entry :
        std::filesystem::directory_iterator(sessions_dir(), ec)) {
        if (ec || !entry.is_regular_file()
            || entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream file(entry.path());
        std::stringstream text;
        text << file.rdbuf();
        const Json::Value root = parse_json(text.str());
        if (!root.isObject()) {
            continue;
        }
        std::string title = root.get("title", "").asString();
        if (title.empty()) {
            title = "Untitled session";
        }
        out.push_back({ entry.path(), std::move(title),
            root.get("saved_at", "").asString() });
    }
    std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
        return left.path.filename() > right.path.filename();
    });
    return out;
}

} // namespace ursa
