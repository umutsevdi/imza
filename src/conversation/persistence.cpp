#include "conversation/persistence.h"

#include "common/util.h"
#include "conversation/session.h"
#include "network/json_io.h"
#include "platform/file_lock.h"
#include "platform/json_file.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <utility>

namespace imza {

namespace {

    constexpr std::string_view INDEX_FILENAME = ".index.json";

    std::filesystem::path index_path()
    {
        return sessions_dir() / INDEX_FILENAME;
    }

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

    Json::Value consume_string(std::string& source)
    {
        std::string owned = std::move(source);
        return Json::Value(owned);
    }

    Json::Value todo_json(TodoList& todo)
    {
        Json::Value out(Json::arrayValue);
        for (auto& item : todo.items) {
            Json::Value value;
            value["content"] = consume_string(item.content);
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

    Json::Value item_json(ConversationItem& item)
    {
        Json::Value out;
        if (auto* user = std::get_if<UserTurn>(&item)) {
            out["type"] = "user";
            out["text"] = consume_string(user->text);
            Json::Value attachments(Json::arrayValue);
            for (auto& attachment : user->attachments) {
                Json::Value value;
                value["path"]    = consume_string(attachment.path);
                value["content"] = consume_string(attachment.content);
                attachments.append(std::move(value));
            }
            out["attachments"] = std::move(attachments);
        } else if (auto* assistant = std::get_if<AssistantTurn>(&item)) {
            out["type"]      = "assistant";
            out["markdown"]  = consume_string(assistant->markdown);
            out["reasoning"] = consume_string(assistant->reasoning);
            out["reasoning_signature"]
                = consume_string(assistant->reasoning_signature);
            out["model"] = consume_string(assistant->model);
            out["reasoning_effort"]
                = consume_string(assistant->reasoning_effort);
            if (assistant->reasoning_ms) {
                out["reasoning_ms"] = static_cast<Json::Int64>(
                    assistant->reasoning_ms->count());
            }
        } else if (auto* tool = std::get_if<ToolCall>(&item)) {
            out["type"]    = "tool";
            out["id"]      = static_cast<Json::UInt64>(tool->id);
            out["call_id"] = consume_string(tool->call_id);
            out["name"]    = consume_string(tool->name);
            out["args"]    = consume_string(tool->args);
            if (!tool->subagent_chats.empty()) {
                Json::Value chats(Json::arrayValue);
                for (SubagentChat& chat : tool->subagent_chats) {
                    Json::Value value;
                    value["title"]      = consume_string(chat.title);
                    value["transcript"] = consume_string(chat.transcript);
                    chats.append(std::move(value));
                }
                out["subagent_chats"] = std::move(chats);
            }
            if (tool->result) {
                out["result_kind"] = static_cast<int>(tool->result->kind);
                out["result"]      = consume_string(tool->result->text);
                if (tool->result->diff) {
                    Json::Value diff;
                    diff["file"] = consume_string(tool->result->diff->file);
                    Json::Value rows(Json::arrayValue);
                    for (auto& row : tool->result->diff->rows) {
                        Json::Value value;
                        value["kind"]  = static_cast<int>(row.kind);
                        value["left"]  = consume_string(row.left);
                        value["right"] = consume_string(row.right);
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
        } else if (auto* todo = std::get_if<TodoList>(&item)) {
            out["type"]  = "todo";
            out["items"] = todo_json(*todo);
        } else if (const auto* event = std::get_if<CompactionEvent>(&item)) {
            out["type"]   = "compaction";
            out["id"]     = static_cast<Json::UInt64>(event->id);
            out["status"] = static_cast<int>(event->status);
        } else if (auto* answer = std::get_if<ModalAnswer>(&item)) {
            out["type"] = "modal_answer";
            Json::Value cards(Json::arrayValue);
            for (auto& card : answer->cards) {
                Json::Value value;
                value["prompt"]    = consume_string(card.prompt);
                value["free_text"] = consume_string(card.free_text);
                Json::Value selected(Json::arrayValue);
                for (auto& choice : card.selected) {
                    selected.append(consume_string(choice));
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
                if (!chat.isObject()) {
                    continue;
                }
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

    void sort_sessions(std::vector<SavedSession>& sessions)
    {
        std::sort(sessions.begin(), sessions.end(),
            [](const auto& left, const auto& right) {
                return left.path.filename() > right.path.filename();
            });
    }

    std::optional<std::vector<SavedSession>> read_index()
    {
        std::ifstream file(index_path(), std::ios::binary);
        if (!file) {
            return std::nullopt;
        }
        std::stringstream text;
        text << file.rdbuf();
        const Json::Value root = parse_json(text.str());
        if (!root.isObject() || root.get("version", 0).asInt() != 1
            || !root["sessions"].isArray()) {
            return std::nullopt;
        }
        std::vector<SavedSession> sessions;
        std::set<std::string> seen;
        for (const Json::Value& value : root["sessions"]) {
            if (!value.isObject() || !value["file"].isString()
                || !value["title"].isString()
                || !value["saved_at"].isString()) {
                return std::nullopt;
            }
            const std::filesystem::path file_name = value["file"].asString();
            if (file_name.empty() || file_name != file_name.filename()
                || file_name.extension() != ".json"
                || file_name == INDEX_FILENAME
                || !seen.insert(file_name.string()).second) {
                return std::nullopt;
            }
            sessions.push_back({ sessions_dir() / file_name,
                value["title"].asString(), value["saved_at"].asString() });
        }
        sort_sessions(sessions);
        return sessions;
    }

    Status write_index(const std::vector<SavedSession>& sessions)
    {
        Json::Value root;
        root["version"] = 1;
        Json::Value entries(Json::arrayValue);
        for (const SavedSession& session : sessions) {
            Json::Value value;
            value["file"]     = session.path.filename().string();
            value["title"]    = session.title;
            value["saved_at"] = session.saved_at;
            entries.append(std::move(value));
        }
        root["sessions"] = std::move(entries);
        return write_json_file(index_path(), root, "");
    }

    std::set<std::string> session_files()
    {
        std::set<std::string> files;
        std::error_code ec;
        if (!std::filesystem::exists(sessions_dir(), ec)) {
            return files;
        }
        for (const auto& entry :
            std::filesystem::directory_iterator(sessions_dir(), ec)) {
            if (ec || !entry.is_regular_file()
                || entry.path().extension() != ".json"
                || entry.path().filename() == INDEX_FILENAME) {
                continue;
            }
            files.insert(entry.path().filename().string());
        }
        return files;
    }

    std::optional<SavedSession> read_session_metadata(
        const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::nullopt;
        }
        std::stringstream text;
        text << file.rdbuf();
        const Json::Value root = parse_json(text.str());
        if (!root.isObject() || !root["items"].isArray()) {
            return std::nullopt;
        }
        std::string title = root.get("title", "").asString();
        if (title.empty()) {
            title = "Untitled session";
        }
        return SavedSession { path, std::move(title),
            root.get("saved_at", "").asString() };
    }

    std::vector<SavedSession> reconcile_index(
        std::optional<SavedSession> added = std::nullopt)
    {
        std::vector<SavedSession> sessions;
        {
            auto lock = acquire_file_lock(lock_path_for(index_path()));
            if (!std::holds_alternative<FileLock>(lock)) {
                for (const std::string& file : session_files()) {
                    if (auto metadata
                        = read_session_metadata(sessions_dir() / file)) {
                        sessions.push_back(std::move(*metadata));
                    }
                }
                sort_sessions(sessions);
                return sessions;
            }
            const std::set<std::string> files = session_files();
            std::map<std::string, SavedSession> entries;
            if (auto indexed = read_index()) {
                for (SavedSession& session : *indexed) {
                    const std::string file = session.path.filename().string();
                    if (files.contains(file)) {
                        entries.emplace(file, std::move(session));
                    }
                }
            }
            if (added) {
                const std::string file = added->path.filename().string();
                if (files.contains(file)) {
                    entries.insert_or_assign(file, std::move(*added));
                }
            }
            for (const std::string& file : files) {
                if (entries.contains(file)) {
                    continue;
                }
                if (auto metadata
                    = read_session_metadata(sessions_dir() / file)) {
                    entries.emplace(file, std::move(*metadata));
                }
            }
            sessions.reserve(entries.size());
            for (auto& [file, session] : entries) {
                sessions.push_back(std::move(session));
            }
            sort_sessions(sessions);
            write_index(sessions);
        }
        return sessions;
    }

} // namespace

Status save_session(Session& session)
{
    std::optional<SessionSnapshot> pending = session.snapshot_for_save();
    if (!pending) {
        return Status::OK;
    }
    SessionSnapshot snapshot = std::move(*pending);
    Json::Value root;
    std::error_code workspace_ec;
    const std::filesystem::path workspace
        = std::filesystem::current_path(workspace_ec);
    if (workspace_ec) {
        return Status::CONFIG_ERROR;
    }
    root["version"] = 1;
    const std::string title
        = snapshot.title.empty() ? "Untitled session" : snapshot.title;
    const std::string saved_at = format_local_time("%Y-%m-%d %H:%M:%S");
    root["title"]              = title;
    root["saved_at"]           = saved_at;
    root["todo"]               = todo_json(snapshot.todo);
    root["compacted_summary"]  = consume_string(snapshot.compacted_summary);
    root["compacted_item_count"]
        = static_cast<Json::UInt64>(snapshot.compacted_item_count);
    root["mode"]      = snapshot.plan_mode ? "plan" : "build";
    root["workspace"] = utf8_from_path(workspace);
    Json::Value items(Json::arrayValue);
    for (auto& item : snapshot.items) {
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
    const SavedSession saved { path, title, saved_at };
    {
        auto lock = acquire_file_lock(lock_path_for(index_path()));
        if (std::holds_alternative<FileLock>(lock)) {
            if (auto indexed = read_index()) {
                indexed->push_back(saved);
                sort_sessions(*indexed);
                write_index(*indexed);
                return Status::OK;
            }
        }
    }
    reconcile_index(saved);
    return Status::OK;
}

Status read_session(const std::filesystem::path& path, LoadedSession& loaded)
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
    std::error_code workspace_ec;
    std::filesystem::path workspace
        = std::filesystem::current_path(workspace_ec);
    if (workspace_ec) {
        return Status::CONFIG_ERROR;
    }
    if (root["workspace"].isString()) {
        const std::filesystem::path saved
            = path_from_utf8(root["workspace"].asString());
        if (std::filesystem::is_directory(saved, workspace_ec)
            && !workspace_ec) {
            workspace = saved;
        }
    }
    SessionSnapshot snapshot;
    try {
        snapshot.persistence = PersistedSession { path };
        snapshot.title       = root.get("title", "").asString();
        snapshot.todo        = parse_todo(root["todo"]);
        snapshot.compacted_summary
            = root.get("compacted_summary", "").asString();
        snapshot.compacted_item_count
            = root.get("compacted_item_count", 0).asUInt64();
        snapshot.plan_mode = root.get("mode", "plan").asString() != "build";
        for (const auto& value : root["items"]) {
            if (auto item = parse_item(value)) {
                snapshot.items.push_back(std::move(*item));
            }
        }
    } catch (const Json::Exception&) {
        return Status::JSON_ERROR;
    }
    loaded = LoadedSession { std::move(snapshot), std::move(workspace) };
    return Status::OK;
}

Status load_session(const std::filesystem::path& path, Session& session,
    std::filesystem::path* workspace)
{
    LoadedSession loaded;
    const Status status = read_session(path, loaded);
    if (status != Status::OK) {
        return status;
    }
    if (workspace != nullptr) {
        *workspace = loaded.workspace;
    }
    session.restore(std::move(loaded.snapshot));
    return Status::OK;
}

std::vector<SavedSession> saved_sessions()
{
    if (auto indexed = read_index()) {
        return *indexed;
    }
    return reconcile_index();
}

DeleteSessionResult delete_saved_session(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path root
        = std::filesystem::weakly_canonical(sessions_dir(), ec);
    const std::filesystem::path target
        = std::filesystem::weakly_canonical(path, ec);
    if (ec || target.parent_path() != root || target.extension() != ".json"
        || target.filename() == INDEX_FILENAME) {
        return DeleteSessionResult::INVALID_PATH;
    }
    if (!std::filesystem::remove(target, ec) || ec) {
        return DeleteSessionResult::REMOVE_FAILED;
    }
    {
        auto lock = acquire_file_lock(lock_path_for(index_path()));
        if (std::holds_alternative<FileLock>(lock)) {
            if (auto indexed = read_index()) {
                indexed->erase(std::remove_if(indexed->begin(), indexed->end(),
                                   [&](const SavedSession& session) {
                                       return session.path == target;
                                   }),
                    indexed->end());
                write_index(*indexed);
                return DeleteSessionResult::OK;
            }
        }
    }
    reconcile_index();
    return DeleteSessionResult::OK;
}

} // namespace imza
