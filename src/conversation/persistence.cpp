#include "conversation/persistence.h"

#include "common/util.h"
#include "conversation/session.h"
#include "network/json_io.h"
#include "platform/file_lock.h"
#include "platform/json_file.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace imza {

namespace {

    constexpr std::string_view INDEX_FILENAME = ".index.json";
    constexpr const char* UNTITLED_TITLE      = "Untitled session";

    std::filesystem::path index_path()
    {
        return sessions_dir() / INDEX_FILENAME;
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

    Json::Value diff_json(DiffView& diff)
    {
        Json::Value out;
        out["file"] = consume_string(diff.file);
        Json::Value rows(Json::arrayValue);
        for (auto& row : diff.rows) {
            Json::Value value;
            value["kind"]  = static_cast<int>(row.kind);
            value["left"]  = consume_string(row.left);
            value["right"] = consume_string(row.right);
            if (row.left_no) {
                value["left_no"] = static_cast<Json::UInt64>(*row.left_no);
            }
            if (row.right_no) {
                value["right_no"] = static_cast<Json::UInt64>(*row.right_no);
            }
            rows.append(std::move(value));
        }
        out["rows"] = std::move(rows);
        return out;
    }

    DiffView parse_diff(const Json::Value& value)
    {
        DiffView diff;
        diff.file = value.get("file", "").asString();
        for (const auto& row_value : value["rows"]) {
            DiffRow row;
            const int row_kind = row_value.get("kind", 0).asInt();
            // SKIP (3) is new; older or foreign values clamp to SAME so
            // old sessions still load.
            if (row_kind >= 0
                && row_kind <= static_cast<int>(DiffRow::Kind::SKIP)) {
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
        return diff;
    }

    Json::Value canvas_json(CanvasView& canvas)
    {
        Json::Value out;
        out["kind"]  = static_cast<int>(canvas.kind);
        out["title"] = consume_string(canvas.title);
        Json::Value series(Json::arrayValue);
        for (auto& entry : canvas.series) {
            Json::Value value;
            value["label"] = consume_string(entry.label);
            Json::Value values(Json::arrayValue);
            for (double number : entry.values) {
                values.append(number);
            }
            value["values"] = std::move(values);
            series.append(std::move(value));
        }
        out["series"] = std::move(series);
        Json::Value grid(Json::arrayValue);
        for (auto& row : canvas.grid) {
            Json::Value values(Json::arrayValue);
            for (double number : row) {
                values.append(number);
            }
            grid.append(std::move(values));
        }
        out["grid"] = std::move(grid);
        return out;
    }

    std::optional<CanvasView> parse_canvas(const Json::Value& value)
    {
        const int kind = value.get("kind", -1).asInt();
        // Unknown kinds cannot render; skip them so older or foreign
        // sessions still load.
        if (kind < 0 || kind > static_cast<int>(CanvasView::Kind::SURFACE)) {
            return std::nullopt;
        }
        CanvasView canvas;
        canvas.kind  = static_cast<CanvasView::Kind>(kind);
        canvas.title = value.get("title", "").asString();
        for (const Json::Value& entry : value["series"]) {
            CanvasSeries series;
            series.label = entry.get("label", "").asString();
            for (const Json::Value& number : entry["values"]) {
                if (number.isNumeric()) {
                    series.values.push_back(number.asDouble());
                }
            }
            canvas.series.push_back(std::move(series));
        }
        for (const Json::Value& row : value["grid"]) {
            std::vector<double> values;
            for (const Json::Value& number : row) {
                if (number.isNumeric()) {
                    values.push_back(number.asDouble());
                }
            }
            canvas.grid.push_back(std::move(values));
        }
        return canvas;
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
                value["path"]       = consume_string(attachment.path);
                value["type"]       = attachment.type_name();
                value["media_type"] = consume_string(attachment.media_type);
                if (attachment.type == Attachment::Type::TEXT) {
                    value["content"] = consume_string(attachment.content);
                } else {
                    value["content"] = base64_encode(attachment.content);
                    attachment.content.clear();
                }
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
                if (tool->result->return_value) {
                    out["return_value"]
                        = std::move(*tool->result->return_value);
                }
                if (!tool->result->diffs.empty()) {
                    Json::Value diffs(Json::arrayValue);
                    for (auto& diff : tool->result->diffs) {
                        diffs.append(diff_json(diff));
                    }
                    out["diffs"] = std::move(diffs);
                }
                if (!tool->result->canvases.empty()) {
                    Json::Value canvases(Json::arrayValue);
                    for (auto& canvas : tool->result->canvases) {
                        canvases.append(canvas_json(canvas));
                    }
                    out["canvases"] = std::move(canvases);
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
                if (!tool->result->dispatch_log.empty()) {
                    Json::Value log(Json::arrayValue);
                    for (const LuaBindingCall& call :
                        tool->result->dispatch_log) {
                        Json::Value entry;
                        entry["binding"] = call.binding;
                        entry["target"]  = call.target;
                        entry["ok"]      = call.ok;
                        log.append(std::move(entry));
                    }
                    out["dispatch_log"] = std::move(log);
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
                Attachment attachment;
                attachment.path = entry.get("path", "").asString();
                if (!entry.isMember("type")) {
                    attachment.content = entry.get("content", "").asString();
                    user.attachments.push_back(std::move(attachment));
                    continue;
                }
                const auto type = Attachment::parse_type(
                    entry.get("type", "text").asString());
                if (!type) {
                    continue;
                }
                attachment.type = *type;
                if (*type == Attachment::Type::TEXT) {
                    attachment.content = entry.get("content", "").asString();
                } else {
                    const auto content
                        = base64_decode(entry.get("content", "").asString());
                    if (!content) {
                        continue;
                    }
                    attachment.content = *content;
                    attachment.media_type
                        = entry.get("media_type", "").asString();
                }
                user.attachments.push_back(std::move(attachment));
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
                    if (value.isMember("return_value")) {
                        tool.result->return_value = value["return_value"];
                    }
                    if (value["diffs"].isArray()) {
                        for (const Json::Value& entry : value["diffs"]) {
                            tool.result->diffs.push_back(parse_diff(entry));
                        }
                    }
                    if (value["canvases"].isArray()) {
                        for (const Json::Value& entry : value["canvases"]) {
                            if (auto canvas = parse_canvas(entry)) {
                                tool.result->canvases.push_back(
                                    std::move(*canvas));
                            }
                        }
                    }
                    if (value.isMember("shell_exit")) {
                        tool.result->shell_status
                            = ShellExit { value["shell_exit"].asInt() };
                    } else if (value.isMember("shell_timeout")) {
                        tool.result->shell_status
                            = ShellTimeout { std::chrono::seconds(
                                value["shell_timeout"].asInt64()) };
                    }
                    if (value["dispatch_log"].isArray()) {
                        for (const auto& entry : value["dispatch_log"]) {
                            tool.result->dispatch_log.push_back(
                                { entry.get("binding", "").asString(),
                                    entry.get("target", "").asString(),
                                    entry.get("ok", true).asBool() });
                        }
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
        const std::optional<Json::Value> stored = read_json_file(index_path());
        if (!stored) {
            return std::nullopt;
        }
        const Json::Value& root = *stored;
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

    // Applies mutate to the freshly-read index under the index lock and
    // writes the result. Entries whose session file no longer exists are
    // dropped, so the fast paths keep repairing the index like reconcile
    // does. Returns false — leaving reconciliation to the caller — when the
    // lock is unavailable, the stored index is malformed, or the write
    // fails.
    bool mutate_index(
        const std::function<bool(std::vector<SavedSession>&)>& mutate)
    {
        auto lock = acquire_file_lock(lock_path_for(index_path()));
        if (!std::holds_alternative<FileLock>(lock)) {
            return false;
        }
        auto indexed = read_index();
        if (!indexed) {
            return false;
        }
        const std::set<std::string> files = session_files();
        std::erase_if(*indexed, [&](const SavedSession& session) {
            return !files.contains(session.path.filename().string());
        });
        if (!mutate(*indexed)) {
            return false;
        }
        return write_index(*indexed) == Status::OK;
    }

    std::optional<SavedSession> read_session_metadata(
        const std::filesystem::path& path)
    {
        const std::optional<Json::Value> stored = read_json_file(path);
        if (!stored) {
            return std::nullopt;
        }
        const Json::Value& root = *stored;
        if (!root.isObject() || !root["items"].isArray()) {
            return std::nullopt;
        }
        std::string title = root.get("title", "").asString();
        if (title.empty()) {
            title = UNTITLED_TITLE;
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
        = snapshot.title.empty() ? UNTITLED_TITLE : snapshot.title;
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
        // A still-streaming tool call has no complete arguments and must
        // never be written out.
        if (const auto* tool = std::get_if<ToolCall>(&item);
            tool != nullptr && tool->phase == ToolCall::Phase::PLANNING) {
            continue;
        }
        items.append(item_json(item));
    }
    root["items"] = std::move(items);

    const std::filesystem::path path
        = sessions_dir() / (session.session_id() + ".json");

    const Status st = write_json_file(path, root, "");
    if (st != Status::OK) {
        return st;
    }
    session.set_persistence(PersistedSession { path });
    const SavedSession saved { path, title, saved_at };
    if (mutate_index([&](std::vector<SavedSession>& indexed) {
            // A re-save of the same run rewrites the same file, so replace
            // its index entry instead of duplicating it.
            const std::string file = saved.path.filename().string();
            std::erase_if(indexed, [&](const SavedSession& entry) {
                return entry.path.filename().string() == file;
            });
            indexed.push_back(saved);
            sort_sessions(indexed);
            return true;
        })) {
        return Status::OK;
    }
    reconcile_index(saved);
    return Status::OK;
}

Status read_session(const std::filesystem::path& path, LoadedSession& loaded)
{
    // Never blocking: an exclusive holder is a chat-active session in
    // another imza process, and waiting on it would freeze the loader.
    auto guard = acquire_file_lock(
        lock_path_for(path), FileLockRequest { FileLockMode::SHARED, false });
    if (!std::holds_alternative<FileLock>(guard)) {
        return Status::CONFIG_ERROR;
    }
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        return Status::CONFIG_ERROR;
    }
    const Json::Value root = parse_json(*text);
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

std::vector<SavedSession> saved_sessions()
{
    if (auto indexed = read_index()) {
        return *indexed;
    }
    return reconcile_index();
}

bool session_file_locked(const std::filesystem::path& path)
{
    return file_lock_held(lock_path_for(path));
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
    // An exclusive holder (chat-active session in another imza process)
    // makes the deletion fail instead of yanking the file from under it.
    auto guard = acquire_file_lock(lock_path_for(target),
        FileLockRequest { FileLockMode::EXCLUSIVE, false });
    if (!std::holds_alternative<FileLock>(guard)) {
        return DeleteSessionResult::REMOVE_FAILED;
    }
    if (!std::filesystem::remove(target, ec) || ec) {
        return DeleteSessionResult::REMOVE_FAILED;
    }
    {
        if (mutate_index([&](std::vector<SavedSession>& indexed) {
                std::erase_if(indexed, [&](const SavedSession& session) {
                    return session.path == target;
                });
                return true;
            })) {
            return DeleteSessionResult::OK;
        }
    }
    reconcile_index();
    return DeleteSessionResult::OK;
}

} // namespace imza
