#include "conversation/persistence.h"

#include "common/util.h"
#include "conversation/session.h"
#include "network/json.h"
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

    std::filesystem::path index_path()
    {
        return sessions_dir() / INDEX_FILENAME;
    }

    // --- Wire-shaped transcript structs ---------------------------------
    // One StoredItem struct is the union of all item kinds: the "type"
    // discriminator picks the members that matter. Optional members give
    // old-transcript tolerance (missing fields default) while glaze
    // handles the parse/serialize mechanically. Numeric enum values are
    // range-checked after the parse so foreign or future values degrade
    // gracefully instead of failing the load.

    struct StoredAttachment {
        std::string path;
        std::optional<std::string> type;
        std::optional<std::string> media_type;
        std::string content;
    };

    struct StoredTodoItem {
        std::string content;
        int status = 0;
    };

    struct StoredDiffRow {
        int kind = 0;
        std::optional<std::uint64_t> left_no;
        std::optional<std::uint64_t> right_no;
        std::string left;
        std::string right;
    };

    struct StoredDiff {
        std::string file;
        std::vector<StoredDiffRow> rows;
    };

    struct StoredSeries {
        std::string label;
        std::vector<double> values;
    };

    struct StoredCanvas {
        int kind = -1;
        std::string title;
        std::vector<StoredSeries> series;
        std::vector<std::vector<double>> grid;
    };

    struct StoredChat {
        std::string title;
        std::string transcript;
    };

    struct StoredCard {
        std::string prompt;
        std::string free_text;
        std::vector<std::string> selected;
    };

    struct StoredDispatch {
        std::string binding;
        std::string target;
        std::optional<bool> ok;
    };

    struct StoredItem {
        std::string type;
        std::optional<std::string> text;
        std::optional<std::vector<StoredAttachment>> attachments;
        std::optional<std::string> markdown;
        std::optional<std::string> reasoning;
        std::optional<std::string> reasoning_signature;
        std::optional<std::int64_t> reasoning_ms;
        std::optional<std::string> model;
        std::optional<std::string> reasoning_effort;
        std::optional<std::int64_t> id;
        std::optional<std::string> call_id;
        std::optional<std::string> name;
        std::optional<std::string> args;
        std::optional<int> result_kind;
        std::optional<std::vector<StoredChat>> subagent_chats;
        // Flatten the result object: the wire stores result fields at the
        // item level, so mirror them directly.
        std::optional<std::string> result;
        std::optional<JsonValue> return_value;
        std::optional<std::vector<StoredDiff>> diffs;
        std::optional<std::vector<StoredCanvas>> canvases;
        std::optional<int> shell_exit;
        std::optional<int> shell_timeout;
        std::optional<std::vector<StoredDispatch>> dispatch_log;
        std::optional<std::vector<StoredTodoItem>> items;
        std::optional<int> status;
        std::optional<std::vector<StoredCard>> cards;
    };

    struct StoredSessionDoc {
        double version = 1.0;
        std::string title;
        std::string saved_at;
        std::optional<std::vector<StoredTodoItem>> todo;
        std::optional<std::vector<std::string>> plans;
        std::optional<std::string> compacted_summary;
        std::optional<std::int64_t> compacted_item_count;
        std::optional<std::string> mode;
        std::optional<std::string> workspace;
        std::optional<std::vector<StoredItem>> items;
    };

    StoredAttachment to_stored(const Attachment& attachment)
    {
        StoredAttachment stored;
        stored.path       = attachment.path;
        stored.type       = attachment.type_name();
        stored.media_type = attachment.media_type.empty()
            ? std::nullopt
            : std::optional<std::string>(attachment.media_type);
        stored.content    = attachment.type == Attachment::Type::TEXT
            ? attachment.content
            : base64_encode(attachment.content);
        return stored;
    }

    std::optional<StoredCanvas> to_stored(const CanvasView& canvas)
    {
        StoredCanvas stored;
        stored.kind  = static_cast<int>(canvas.kind);
        stored.title = canvas.title;
        for (const auto& entry : canvas.series) {
            stored.series.push_back({ entry.label, entry.values });
        }
        stored.grid = canvas.grid;
        return stored;
    }

    std::optional<CanvasView> from_stored(const StoredCanvas& stored)
    {
        if (stored.kind < 0
            || stored.kind > static_cast<int>(CanvasView::Kind::SURFACE)) {
            // Unknown kinds cannot render; skip them so older or foreign
            // sessions still load.
            return std::nullopt;
        }
        CanvasView canvas;
        canvas.kind  = static_cast<CanvasView::Kind>(stored.kind);
        canvas.title = stored.title;
        for (const auto& entry : stored.series) {
            CanvasSeries series;
            series.label  = entry.label;
            series.values = entry.values;
            canvas.series.push_back(std::move(series));
        }
        canvas.grid = stored.grid;
        return canvas;
    }

    StoredDiff to_stored(const DiffView& diff)
    {
        StoredDiff stored;
        stored.file = diff.file;
        for (const auto& row : diff.rows) {
            StoredDiffRow stored_row;
            stored_row.kind     = static_cast<int>(row.kind);
            stored_row.left_no  = row.left_no;
            stored_row.right_no = row.right_no;
            stored_row.left     = row.left;
            stored_row.right    = row.right;
            stored.rows.push_back(std::move(stored_row));
        }
        return stored;
    }

    DiffView from_stored(const StoredDiff& stored)
    {
        DiffView diff;
        diff.file = stored.file;
        for (const auto& stored_row : stored.rows) {
            DiffRow row;
            // SKIP (3) is new; older or foreign values clamp to SAME so
            // old sessions still load.
            if (stored_row.kind >= 0
                && stored_row.kind <= static_cast<int>(DiffRow::Kind::SKIP)) {
                row.kind = static_cast<DiffRow::Kind>(stored_row.kind);
            }
            row.left_no  = stored_row.left_no;
            row.right_no = stored_row.right_no;
            row.left     = stored_row.left;
            row.right    = stored_row.right;
            diff.rows.push_back(std::move(row));
        }
        return diff;
    }

    std::optional<StoredItem> to_stored(const ConversationItem& item)
    {
        StoredItem stored;
        if (const auto* user = std::get_if<UserTurn>(&item)) {
            stored.type = "user";
            stored.text = user->text;
            std::vector<StoredAttachment> attachments;
            attachments.reserve(user->attachments.size());
            for (const auto& attachment : user->attachments) {
                attachments.push_back(to_stored(attachment));
            }
            if (!attachments.empty()) {
                stored.attachments = std::move(attachments);
            }
        } else if (const auto* assistant = std::get_if<AssistantTurn>(&item)) {
            stored.type                = "assistant";
            stored.markdown            = assistant->markdown;
            stored.reasoning           = assistant->reasoning;
            stored.reasoning_signature = assistant->reasoning_signature;
            stored.model               = assistant->model;
            stored.reasoning_effort    = assistant->reasoning_effort;
            if (assistant->reasoning_ms) {
                stored.reasoning_ms = assistant->reasoning_ms->count();
            }
        } else if (const auto* tool = std::get_if<ToolCall>(&item)) {
            stored.type    = "tool";
            stored.id      = static_cast<std::int64_t>(tool->id);
            stored.call_id = tool->call_id;
            stored.name    = tool->name;
            stored.args    = tool->args;
            if (!tool->subagent_chats.empty()) {
                std::vector<StoredChat> chats;
                chats.reserve(tool->subagent_chats.size());
                for (const auto& chat : tool->subagent_chats) {
                    chats.push_back({ chat.title, chat.transcript });
                }
                stored.subagent_chats = std::move(chats);
            }
            if (tool->result) {
                stored.result = tool->result->text;
                if (tool->result->return_value) {
                    stored.return_value = *tool->result->return_value;
                }
                if (!tool->result->diffs.empty()) {
                    std::vector<StoredDiff> diffs;
                    diffs.reserve(tool->result->diffs.size());
                    for (const auto& diff : tool->result->diffs) {
                        diffs.push_back(to_stored(diff));
                    }
                    stored.diffs = std::move(diffs);
                }
                if (!tool->result->canvases.empty()) {
                    std::vector<StoredCanvas> canvases;
                    canvases.reserve(tool->result->canvases.size());
                    for (const auto& canvas : tool->result->canvases) {
                        if (auto stored_canvas = to_stored(canvas)) {
                            canvases.push_back(std::move(*stored_canvas));
                        }
                    }
                    if (!canvases.empty()) {
                        stored.canvases = std::move(canvases);
                    }
                }
                if (tool->result->shell_status) {
                    std::visit(
                        [&](const auto& status) {
                            using T = std::decay_t<decltype(status)>;
                            if constexpr (std::is_same_v<T, ShellExit>) {
                                stored.shell_exit = status.code;
                            } else {
                                stored.shell_timeout
                                    = static_cast<int>(status.duration.count());
                            }
                        },
                        *tool->result->shell_status);
                }
                if (!tool->result->dispatch_log.empty()) {
                    std::vector<StoredDispatch> log;
                    log.reserve(tool->result->dispatch_log.size());
                    for (const auto& call : tool->result->dispatch_log) {
                        log.push_back({ call.binding, call.target, call.ok });
                    }
                    stored.dispatch_log = std::move(log);
                }
                // result_kind is the presence marker for the result fields.
                stored.result_kind = static_cast<int>(tool->result->kind);
            }
        } else if (const auto* todo = std::get_if<TodoList>(&item)) {
            stored.type = "todo";
            std::vector<StoredTodoItem> items;
            items.reserve(todo->items.size());
            for (const auto& todo_item : todo->items) {
                items.push_back(
                    { todo_item.content, static_cast<int>(todo_item.status) });
            }
            stored.items = std::move(items);
        } else if (const auto* event = std::get_if<CompactionEvent>(&item)) {
            stored.type   = "compaction";
            stored.id     = static_cast<std::int64_t>(event->id);
            stored.status = static_cast<int>(event->status);
        } else if (const auto* answer = std::get_if<ModalAnswer>(&item)) {
            stored.type = "modal_answer";
            std::vector<StoredCard> cards;
            cards.reserve(answer->cards.size());
            for (const auto& card : answer->cards) {
                cards.push_back({ card.prompt, card.free_text, card.selected });
            }
            stored.cards = std::move(cards);
        } else {
            return std::nullopt;
        }
        return stored;
    }

    std::optional<ConversationItem> from_stored(const StoredItem& stored)
    {
        if (stored.type == "user") {
            UserTurn user;
            user.text = stored.text.value_or("");
            if (stored.attachments) {
                for (const auto& entry : *stored.attachments) {
                    Attachment attachment;
                    attachment.path = entry.path;
                    // No "type" member means a legacy TEXT attachment.
                    const auto parsed_type = Attachment::parse_type(
                        entry.type.value_or("").empty() ? "text" : *entry.type);
                    if (!parsed_type) {
                        continue;
                    }
                    attachment.type = *parsed_type;
                    if (*parsed_type == Attachment::Type::TEXT) {
                        attachment.content = entry.content;
                    } else {
                        const auto content = base64_decode(entry.content);
                        if (!content) {
                            continue;
                        }
                        attachment.content    = *content;
                        attachment.media_type = entry.media_type.value_or("");
                    }
                    user.attachments.push_back(std::move(attachment));
                }
            }
            return user;
        }
        if (stored.type == "assistant") {
            AssistantTurn assistant;
            assistant.markdown  = stored.markdown.value_or("");
            assistant.reasoning = stored.reasoning.value_or("");
            assistant.reasoning_signature
                = stored.reasoning_signature.value_or("");
            assistant.model            = stored.model.value_or("");
            assistant.reasoning_effort = stored.reasoning_effort.value_or("");
            if (stored.reasoning_ms) {
                assistant.reasoning_ms
                    = std::chrono::milliseconds(*stored.reasoning_ms);
            }
            return assistant;
        }
        if (stored.type == "tool") {
            ToolCall tool;
            tool.id      = static_cast<std::size_t>(stored.id.value_or(0));
            tool.call_id = stored.call_id.value_or("");
            tool.name    = stored.name.value_or("");
            tool.args    = stored.args.value_or("");
            if (stored.subagent_chats) {
                for (const auto& chat : *stored.subagent_chats) {
                    tool.subagent_chats.push_back(
                        { chat.title.empty() ? "Agent" : chat.title,
                            chat.transcript });
                }
            }
            if (stored.result_kind) {
                if (*stored.result_kind >= 0 && *stored.result_kind <= 3) {
                    ToolCall::Result result;
                    result.kind = static_cast<ToolCall::Result::Kind>(
                        *stored.result_kind);
                    result.text         = stored.result.value_or("");
                    result.return_value = stored.return_value;
                    if (stored.diffs) {
                        for (const auto& diff : *stored.diffs) {
                            result.diffs.push_back(from_stored(diff));
                        }
                    }
                    if (stored.canvases) {
                        for (const auto& canvas : *stored.canvases) {
                            if (auto view = from_stored(canvas)) {
                                result.canvases.push_back(std::move(*view));
                            }
                        }
                    }
                    if (stored.shell_exit) {
                        result.shell_status = ShellExit { *stored.shell_exit };
                    } else if (stored.shell_timeout) {
                        result.shell_status = ShellTimeout {
                            std::chrono::seconds(*stored.shell_timeout)
                        };
                    }
                    if (stored.dispatch_log) {
                        for (const auto& entry : *stored.dispatch_log) {
                            result.dispatch_log.push_back({ entry.binding,
                                entry.target, entry.ok.value_or(true) });
                        }
                    }
                    tool.result = std::move(result);
                }
            }
            return tool;
        }
        if (stored.type == "todo") {
            TodoList todo;
            if (stored.items) {
                for (const auto& entry : *stored.items) {
                    TodoItem item;
                    item.content = entry.content;
                    if (entry.status >= 0 && entry.status <= 3) {
                        item.status
                            = static_cast<TodoItem::Status>(entry.status);
                    }
                    todo.items.push_back(std::move(item));
                }
            }
            return todo;
        }
        if (stored.type == "compaction") {
            CompactionEvent event;
            event.id         = static_cast<std::size_t>(stored.id.value_or(0));
            const int status = stored.status.value_or(1);
            event.status     = status >= 0 && status <= 2
                ? static_cast<CompactionEvent::Status>(status)
                : CompactionEvent::Status::COMPLETED;
            return event;
        }
        if (stored.type == "modal_answer") {
            ModalAnswer answer;
            if (stored.cards) {
                for (const auto& card : *stored.cards) {
                    answer.cards.push_back(
                        { card.selected, card.free_text, card.prompt });
                }
            }
            return answer;
        }
        // Unknown item types cannot render; skip them so older or foreign
        // sessions still load.
        return std::nullopt;
    }

    void sort_sessions(std::vector<SavedSession>& sessions)
    {
        std::sort(sessions.begin(), sessions.end(),
            [](const auto& left, const auto& right) {
                return left.path.filename() > right.path.filename();
            });
    }

    struct StoredIndexEntry {
        std::string file;
        std::string title;
        std::string saved_at;
    };

    struct StoredIndex {
        double version = 0.0;
        std::vector<StoredIndexEntry> sessions;
    };

    std::optional<std::vector<SavedSession>> read_index()
    {
        const std::optional<std::string> stored = read_text_file(index_path());
        if (!stored) {
            return std::nullopt;
        }
        StoredIndex index;
        // A malformed index rebuilds rather than failing the caller.
        if (json_parse_checked(*stored, index)) {
            return std::nullopt;
        }
        if (static_cast<int>(index.version) != 1) {
            return std::nullopt;
        }
        std::vector<SavedSession> sessions;
        std::set<std::string> seen;
        for (const auto& entry : index.sessions) {
            const std::filesystem::path file_name = entry.file;
            if (file_name.empty() || file_name != file_name.filename()
                || file_name.extension() != ".json"
                || file_name == INDEX_FILENAME
                || !seen.insert(file_name.string()).second) {
                return std::nullopt;
            }
            sessions.push_back(
                { sessions_dir() / file_name, entry.title, entry.saved_at });
        }
        sort_sessions(sessions);
        return sessions;
    }

    Status write_index(const std::vector<SavedSession>& sessions)
    {
        StoredIndex index;
        index.version = 1.0;
        index.sessions.reserve(sessions.size());
        for (const SavedSession& session : sessions) {
            index.sessions.push_back({ session.path.filename().string(),
                session.title, session.saved_at });
        }
        auto serialized = json_dump_checked(index);
        if (!serialized) {
            return Status::JSON_ERROR;
        }
        return write_json_file(index_path(), *serialized);
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

    // Entries whose session file no longer exists are dropped, so fast
    // paths keep repairing the index.
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
        const std::optional<std::string> stored = read_text_file(path);
        if (!stored) {
            return std::nullopt;
        }
        StoredSessionDoc doc;
        if (json_parse_checked(*stored, doc)) {
            return std::nullopt;
        }
        std::string title = doc.title;
        if (title.empty()) {
            title = UNTITLED_TITLE;
        }
        return SavedSession { path, std::move(title), doc.saved_at };
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
    std::error_code workspace_ec;
    const std::filesystem::path workspace
        = std::filesystem::current_path(workspace_ec);
    if (workspace_ec) {
        return Status::CONFIG_ERROR;
    }
    StoredSessionDoc doc;
    doc.version  = 1.0;
    doc.title    = snapshot.title.empty() ? std::string { UNTITLED_TITLE }
                                          : snapshot.title;
    doc.saved_at = format_local_time("%Y-%m-%d %H:%M:%S");
    std::vector<StoredTodoItem> todo;
    todo.reserve(snapshot.todo.items.size());
    for (const auto& item : snapshot.todo.items) {
        todo.push_back({ item.content, static_cast<int>(item.status) });
    }
    doc.todo = std::move(todo);
    std::vector<std::string> plans;
    plans.reserve(snapshot.plans.size());
    for (const auto& plan : snapshot.plans) {
        plans.push_back(plan.content);
    }
    doc.plans             = std::move(plans);
    doc.compacted_summary = snapshot.compacted_summary;
    doc.compacted_item_count
        = static_cast<std::int64_t>(snapshot.compacted_item_count);
    doc.mode      = snapshot.plan_mode ? "plan" : "build";
    doc.workspace = utf8_from_path(workspace);
    std::vector<StoredItem> items;
    for (auto& item : snapshot.items) {
        // A still-streaming tool call has no complete arguments and must
        // never be written out.
        if (const auto* tool = std::get_if<ToolCall>(&item);
            tool != nullptr && tool->phase == ToolCall::Phase::PLANNING) {
            continue;
        }
        if (auto stored = to_stored(item)) {
            items.push_back(std::move(*stored));
        }
    }
    doc.items = std::move(items);

    const std::filesystem::path path
        = sessions_dir() / (session.session_id() + ".json");

    auto serialized = json_dump_checked(doc);
    if (!serialized) {
        return Status::JSON_ERROR;
    }
    const Status st = write_json_file(path, *serialized);
    if (st != Status::OK) {
        return st;
    }
    session.set_persistence(PersistedSession { path });
    const SavedSession saved { path, doc.title, doc.saved_at };
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
    StoredSessionDoc doc;
    if (glz::error_ctx error = json_parse_checked(*text, doc)) {
        return Status::JSON_ERROR;
    }
    std::error_code workspace_ec;
    std::filesystem::path workspace
        = std::filesystem::current_path(workspace_ec);
    if (workspace_ec) {
        return Status::CONFIG_ERROR;
    }
    if (doc.workspace && !doc.workspace->empty()) {
        const std::filesystem::path saved = path_from_utf8(*doc.workspace);
        if (std::filesystem::is_directory(saved, workspace_ec)
            && !workspace_ec) {
            workspace = saved;
        }
    }
    SessionSnapshot snapshot;
    snapshot.persistence = PersistedSession { path };
    snapshot.title       = doc.title;
    if (doc.todo) {
        for (const auto& entry : *doc.todo) {
            TodoItem item;
            item.content = entry.content;
            if (entry.status >= 0 && entry.status <= 3) {
                item.status = static_cast<TodoItem::Status>(entry.status);
            }
            snapshot.todo.items.push_back(std::move(item));
        }
    }
    snapshot.compacted_summary = doc.compacted_summary.value_or("");
    snapshot.compacted_item_count
        = static_cast<std::size_t>(doc.compacted_item_count.value_or(0));
    snapshot.plan_mode = doc.mode.value_or("") != "build";
    if (doc.plans) {
        for (const auto& plan : *doc.plans) {
            snapshot.plans.push_back(PlanDoc { plan });
        }
    }
    if (doc.items) {
        for (const auto& stored : *doc.items) {
            // A malformed item (type mismatch where the format has a
            // strict type) fails the whole parse; unknown item kinds are
            // skipped by from_stored so foreign sessions still load.
            if (auto item = from_stored(stored)) {
                snapshot.items.push_back(std::move(*item));
            }
        }
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
