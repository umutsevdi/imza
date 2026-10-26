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
#include <tuple>
#include <type_traits>
#include <utility>

namespace imza {

constexpr std::string_view INDEX_FILENAME = ".index.json";

std::filesystem::path index_path() { return sessions_dir() / INDEX_FILENAME; }

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

struct StoredDispatch {
    std::string binding;
    std::string target;
    std::optional<bool> ok;
};

struct StoredUsageEntry {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::uint64_t> prompt;
    std::optional<std::uint64_t> completion;
    std::optional<std::uint64_t> cached_read;
    std::optional<std::uint64_t> cached_write;
    std::optional<std::uint64_t> total;
    std::optional<double> cost;
    // Epoch seconds; absent in legacy files.
    std::optional<std::int64_t> finished_at;
};

// One "provider + model" row of a session's usage summary.
struct StoredModelUsage {
    std::string provider;
    std::string model;
    std::uint64_t prompt     = 0;
    std::uint64_t completion = 0;
    std::uint64_t total      = 0;
    double cost              = 0.0;
};

struct StoredDayUsage {
    std::string day;
    std::uint64_t total = 0;
    double cost         = 0.0;
    std::optional<std::vector<StoredModelUsage>> by_model;
};

struct StoredUsageSummary {
    std::uint64_t prompt     = 0;
    std::uint64_t completion = 0;
    std::uint64_t total      = 0;
    double cost              = 0.0;
    std::optional<std::vector<StoredModelUsage>> by_model;
    std::optional<std::vector<StoredDayUsage>> by_day;
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
    // Epoch seconds; absent in legacy files (turn predates tracking).
    std::optional<std::int64_t> finished_at;
    std::optional<std::vector<StoredUsageEntry>> usage;
    std::optional<std::int64_t> id;
    std::optional<std::string> call_id;
    std::optional<std::string> name;
    std::optional<std::string> args;
    std::optional<int> result_kind;
    std::optional<std::vector<SubagentChat>> subagent_chats;
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
};

// One entry of the ordered document sequence: kind is "plan", "build",
// or "handoff". Legacy files carry only `plans`.
struct StoredDocument {
    std::string kind;
    std::string content;
};

struct StoredSessionDoc {
    std::string title;
    std::string saved_at;
    std::optional<std::vector<StoredTodoItem>> todo;
    std::optional<std::vector<StoredDocument>> documents;
    std::optional<std::vector<std::string>> plans;
    std::optional<std::string> compacted_summary;
    std::optional<std::int64_t> compacted_item_count;
    std::optional<std::string> mode;
    std::optional<std::string> workspace;
    std::optional<std::vector<StoredItem>> items;
    // Absent fields load as zero so legacy files load.
    std::optional<StoredUsageSummary> usage;
    std::optional<std::vector<StoredUsageEntry>> usage_log;
};

namespace {

    // Project one vector through `map`; nullopt when empty so optional
    // item members stay unset instead of serializing an empty array.
    template <typename In, typename Map>
    auto map_all(const std::vector<In>& items, Map map)
        -> std::optional<std::vector<std::invoke_result_t<Map&, const In&>>>
    {
        if (items.empty()) {
            return std::nullopt;
        }
        std::vector<std::invoke_result_t<Map&, const In&>> mapped;
        mapped.reserve(items.size());
        for (const auto& item : items) {
            mapped.push_back(map(item));
        }
        return mapped;
    }

    TodoItem stored_todo_item(const StoredTodoItem& entry)
    {
        TodoItem item;
        item.content = entry.content;
        // Status integers outside the enum range fall back to pending.
        if (entry.status >= 0 && entry.status <= 3) {
            item.status = static_cast<TodoItem::Status>(entry.status);
        }
        return item;
    }

    StoredUsageEntry to_stored(const UsageEntry& entry)
    {
        StoredUsageEntry stored;
        stored.provider     = entry.provider;
        stored.model        = entry.model;
        stored.prompt       = entry.usage.prompt;
        stored.completion   = entry.usage.completion;
        stored.cached_read  = entry.usage.cached_read;
        stored.cached_write = entry.usage.cached_write;
        stored.total        = entry.usage.total;
        stored.cost         = entry.cost;
        if (entry.finished_at != std::chrono::system_clock::time_point { }) {
            stored.finished_at
                = std::chrono::duration_cast<std::chrono::seconds>(
                    entry.finished_at.time_since_epoch())
                      .count();
        }
        return stored;
    }

    UsageEntry from_stored(const StoredUsageEntry& stored)
    {
        UsageEntry entry;
        entry.provider           = stored.provider.value_or("");
        entry.model              = stored.model.value_or("");
        entry.usage.prompt       = stored.prompt.value_or(0);
        entry.usage.completion   = stored.completion.value_or(0);
        entry.usage.cached_read  = stored.cached_read.value_or(0);
        entry.usage.cached_write = stored.cached_write.value_or(0);
        entry.usage.total        = stored.total.value_or(0);
        entry.cost               = stored.cost.value_or(0.0);
        if (stored.finished_at) {
            entry.finished_at = std::chrono::system_clock::time_point {
                std::chrono::seconds(*stored.finished_at)
            };
        }
        return entry;
    }

    StoredDayUsage to_stored(const UsageDayRow& row)
    {
        StoredDayUsage stored { row.day, row.total, row.cost, std::nullopt };
        if (!row.by_model.empty()) {
            stored.by_model
                = map_all(row.by_model, [](const UsageModelRow& model) {
                      return StoredModelUsage { model.provider, model.model,
                          model.prompt, model.completion, model.total,
                          model.cost };
                  });
        }
        return stored;
    }

    // The one Stored <-> domain pair for summaries; every index and
    // session-file path converts through these.
    StoredUsageSummary to_stored(const UsageSummary& usage)
    {
        StoredUsageSummary stored;
        stored.prompt     = usage.prompt;
        stored.completion = usage.completion;
        stored.total      = usage.total;
        stored.cost       = usage.cost;
        if (!usage.by_model.empty()) {
            stored.by_model
                = map_all(usage.by_model, [](const UsageModelRow& row) {
                      return StoredModelUsage { row.provider, row.model,
                          row.prompt, row.completion, row.total, row.cost };
                  });
        }
        if (!usage.by_day.empty()) {
            stored.by_day = map_all(usage.by_day,
                [](const UsageDayRow& row) { return to_stored(row); });
        }
        return stored;
    }

    StoredUsageSummary to_stored(const GlobalUsageSummary& summary)
    {
        StoredUsageSummary stored;
        stored.prompt     = summary.prompt;
        stored.completion = summary.completion;
        stored.total      = summary.total;
        stored.cost       = summary.total_cost;
        if (!summary.by_model.empty()) {
            stored.by_model
                = map_all(summary.by_model, [](const UsageBreakdownRow& row) {
                      return StoredModelUsage { row.provider, row.model,
                          row.prompt, row.completion, row.total, row.cost };
                  });
        }
        if (!summary.by_day.empty()) {
            stored.by_day = map_all(summary.by_day,
                [](const UsageDayRow& row) { return to_stored(row); });
        }
        return stored;
    }

    UsageSummary from_stored(const StoredUsageSummary& stored)
    {
        UsageSummary usage;
        usage.prompt     = stored.prompt;
        usage.completion = stored.completion;
        usage.total      = stored.total;
        usage.cost       = stored.cost;
        if (stored.by_model) {
            for (const StoredModelUsage& row : *stored.by_model) {
                usage.by_model.push_back({ row.provider, row.model, row.prompt,
                    row.completion, row.total, row.cost });
            }
        }
        if (stored.by_day) {
            for (const StoredDayUsage& row : *stored.by_day) {
                UsageDayRow day { row.day, row.total, row.cost, { } };
                if (row.by_model) {
                    for (const StoredModelUsage& model : *row.by_model) {
                        day.by_model.push_back(
                            { model.provider, model.model, model.prompt,
                                model.completion, model.total, model.cost });
                    }
                }
                usage.by_day.push_back(std::move(day));
            }
        }
        return usage;
    }

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
            : attachment.encoded_or_compute();
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
            stored.attachments
                = map_all(user->attachments, [](const Attachment& attachment) {
                      return to_stored(attachment);
                  });
        } else if (const auto* assistant = std::get_if<AssistantTurn>(&item)) {
            stored.type                = "assistant";
            stored.markdown            = assistant->markdown;
            stored.reasoning           = assistant->reasoning;
            stored.reasoning_signature = assistant->reasoning_signature;
            stored.model               = assistant->model;
            stored.reasoning_effort    = assistant->reasoning_effort;
            if (assistant->finished_at
                != std::chrono::system_clock::time_point { }) {
                stored.finished_at
                    = std::chrono::duration_cast<std::chrono::seconds>(
                        assistant->finished_at.time_since_epoch())
                          .count();
            }
            stored.usage = map_all(assistant->usage,
                [](const UsageEntry& entry) { return to_stored(entry); });
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
                stored.subagent_chats = tool->subagent_chats;
            }
            if (tool->result) {
                stored.result = tool->result->text;
                if (tool->result->return_value) {
                    stored.return_value = *tool->result->return_value;
                }
                stored.diffs = map_all(tool->result->diffs,
                    [](const DiffView& diff) { return to_stored(diff); });
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
                stored.dispatch_log = map_all(
                    tool->result->dispatch_log, [](const LuaBindingCall& call) {
                        return StoredDispatch { call.binding, call.target,
                            call.ok };
                    });
                // result_kind is the presence marker for the result fields.
                stored.result_kind = static_cast<int>(tool->result->kind);
            }
        } else if (const auto* todo = std::get_if<TodoList>(&item)) {
            stored.type  = "todo";
            stored.items = map_all(todo->items, [](const TodoItem& todo_item) {
                return StoredTodoItem { todo_item.content,
                    static_cast<int>(todo_item.status) };
            });
        } else if (const auto* event = std::get_if<CompactionEvent>(&item)) {
            stored.type   = "compaction";
            stored.id     = static_cast<std::int64_t>(event->id);
            stored.status = static_cast<int>(event->status);
        } else if (const auto* notice = std::get_if<Notice>(&item)) {
            stored.type = "notice";
            stored.text = notice->text;
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
                        attachment.encoded    = entry.content;
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
            if (stored.finished_at) {
                assistant.finished_at = std::chrono::system_clock::time_point {
                    std::chrono::seconds(*stored.finished_at)
                };
            }
            if (stored.usage) {
                for (const auto& entry : *stored.usage) {
                    assistant.usage.push_back(from_stored(entry));
                }
            }
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
                    todo.items.push_back(stored_todo_item(entry));
                }
            }
            return todo;
        }
        if (stored.type == "notice") {
            return Notice { stored.text.value_or("") };
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

} // namespace

struct StoredIndexEntry {
    std::string file;
    std::string title;
    std::string saved_at;
    // Absent in legacy indexes; treated as zero.
    std::optional<StoredUsageSummary> usage;
};

struct StoredIndex {
    double version = 0.0;
    std::vector<StoredIndexEntry> sessions;
};

namespace {

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
            SavedSession saved;
            saved.path     = sessions_dir() / file_name;
            saved.title    = entry.title;
            saved.saved_at = entry.saved_at;
            if (entry.usage) {
                saved.usage = from_stored(*entry.usage);
            }
            sessions.push_back(std::move(saved));
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
            StoredIndexEntry entry { session.path.filename().string(),
                session.title, session.saved_at, to_stored(session.usage) };
            index.sessions.push_back(std::move(entry));
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
        SavedSession saved { path, std::move(title), doc.saved_at };
        if (doc.usage) {
            saved.usage = from_stored(*doc.usage);
        }
        return saved;
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
    doc.title     = snapshot.title.empty() ? std::string { UNTITLED_TITLE }
                                           : snapshot.title;
    doc.saved_at  = format_local_time("%Y-%m-%d %H:%M:%S");
    doc.todo      = map_all(snapshot.todo.items, [](const TodoItem& item) {
        return StoredTodoItem { item.content, static_cast<int>(item.status) };
    });
    doc.documents = map_all(snapshot.documents, [](const SessionDoc& entry) {
        return StoredDocument {
            entry.kind == SessionDoc::Kind::BUILD         ? "build"
                : entry.kind == SessionDoc::Kind::HANDOFF ? "handoff"
                                                          : "plan",
            entry.content,
        };
    });
    doc.compacted_summary = snapshot.compacted_summary;
    doc.compacted_item_count
        = static_cast<std::int64_t>(snapshot.compacted_item_count);
    doc.mode      = snapshot.plan_mode ? "plan" : "build";
    doc.workspace = utf8_from_path(workspace);
    doc.usage     = to_stored(summarize_usage(snapshot.usage_log));
    doc.usage_log = map_all(
        snapshot.usage_log, [](const UsageEntry& e) { return to_stored(e); });
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
    const SavedSession saved { path, doc.title, doc.saved_at,
        from_stored(*doc.usage) };
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
    // An exclusive holder is a chat-active session in another imza process;
    // refuse before reading so a switch never gets as far as mutating the
    // active conversation.
    if (foreign_lock_live(lock_path_for(path))) {
        return Status::CONFIG_ERROR;
    }
    // Never blocking: a holder that raced in after the probe still refuses
    // the degraded shared request instead of freezing the loader.
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
            snapshot.todo.items.push_back(stored_todo_item(entry));
        }
    }
    snapshot.compacted_summary = doc.compacted_summary.value_or("");
    snapshot.compacted_item_count
        = static_cast<std::size_t>(doc.compacted_item_count.value_or(0));
    snapshot.plan_mode = doc.mode.value_or("") != "build";
    if (doc.documents) {
        for (const auto& entry : *doc.documents) {
            snapshot.documents.push_back(SessionDoc {
                entry.kind == "build"         ? SessionDoc::Kind::BUILD
                    : entry.kind == "handoff" ? SessionDoc::Kind::HANDOFF
                                              : SessionDoc::Kind::PLAN,
                entry.content,
            });
        }
    } else if (doc.plans) {
        // Legacy files stored plan revisions only.
        for (const auto& plan : *doc.plans) {
            snapshot.documents.push_back(
                SessionDoc { SessionDoc::Kind::PLAN, plan });
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
    if (doc.usage_log) {
        for (const auto& entry : *doc.usage_log) {
            snapshot.usage_log.push_back(from_stored(entry));
        }
    }
    if (doc.usage) {
        snapshot.totals.prompt     = doc.usage->prompt;
        snapshot.totals.completion = doc.usage->completion;
        snapshot.totals.total      = doc.usage->total;
        snapshot.total_cost        = doc.usage->cost;
    } else if (!snapshot.usage_log.empty()) {
        // A log without a summary (crash between writes) still yields
        // correct totals.
        const GlobalUsageSummary derived = summarize_usage(snapshot.usage_log);
        snapshot.totals.prompt           = derived.prompt;
        snapshot.totals.completion       = derived.completion;
        snapshot.totals.total            = derived.total;
        snapshot.total_cost              = derived.total_cost;
    }
    // The last request's usage is the current context size; the log's
    // newest entry restores it exactly.
    if (!snapshot.usage_log.empty()) {
        snapshot.last = snapshot.usage_log.back().usage;
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

// Sorts accumulated rows by provider then model, merging duplicates.
template <typename Row> void merge_rollup_rows(std::vector<Row>& rows)
{
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return std::tie(a.provider, a.model) < std::tie(b.provider, b.model);
    });
    rows.erase(std::unique(rows.begin(), rows.end(),
                   [](const Row& a, const Row& b) {
                       return a.provider == b.provider && a.model == b.model;
                   }),
        rows.end());
}

std::vector<UsageBreakdownRow> rollup_usage_by_model(
    const std::vector<UsageEntry>& entries)
{
    std::map<std::pair<std::string, std::string>, UsageBreakdownRow> rows;
    for (const UsageEntry& entry : entries) {
        auto& row    = rows[{ entry.provider, entry.model }];
        row.provider = entry.provider;
        row.model    = entry.model;
        row.prompt += entry.usage.prompt;
        row.completion += entry.usage.completion;
        row.total += entry.usage.total;
        row.cost += entry.cost;
    }
    std::vector<UsageBreakdownRow> out;
    out.reserve(rows.size());
    for (auto& [key, row] : rows) {
        out.push_back(std::move(row));
    }
    return out;
}

// Spend day of one entry: its own request timestamp, rendered in local
// time so days match what the user saw. Entries that predate
// per-request timestamps (epoch 0) have no day; their caller buckets
// them from the session's save date instead.
bool usage_entry_day(const UsageEntry& entry, std::string& day)
{
    if (entry.finished_at == std::chrono::system_clock::time_point { }) {
        return false;
    }
    day = format_local_time(entry.finished_at, "%Y-%m-%d");
    return true;
}

GlobalUsageSummary summarize_usage(const std::vector<UsageEntry>& entries)
{
    GlobalUsageSummary out;
    std::map<std::string, UsageDayRow> by_day;
    // The day's own model breakdown feeds stacked charts; accumulated
    // per (provider, model) so repeat requests join their segment.
    std::map<std::string,
        std::map<std::pair<std::string, std::string>, UsageModelRow>>
        day_models;
    for (const UsageEntry& entry : entries) {
        out.prompt += entry.usage.prompt;
        out.completion += entry.usage.completion;
        out.total += entry.usage.total;
        out.total_cost += entry.cost;
        // Legacy entries carry no request date; "1970-01-01" is their
        // epoch bucket, sorting before every real day.
        std::string day = "1970-01-01";
        usage_entry_day(entry, day);
        UsageDayRow& row = by_day[day];
        row.day          = day;
        row.total += entry.usage.total;
        row.cost += entry.cost;
        auto& model    = day_models[day][{ entry.provider, entry.model }];
        model.provider = entry.provider;
        model.model    = entry.model;
        model.total += entry.usage.total;
        model.cost += entry.cost;
    }
    out.by_model = rollup_usage_by_model(entries);
    for (auto& [day, row] : by_day) {
        auto& rows = day_models[day];
        row.by_model.reserve(rows.size());
        for (auto& [key, model] : rows) {
            row.by_model.push_back(std::move(model));
        }
        out.by_day.push_back(std::move(row));
    }
    return out;
}

GlobalUsageSummary summarize_saved_usage(const std::vector<SavedSession>& saved)
{
    GlobalUsageSummary out;
    std::map<std::string, UsageDayRow> by_day;
    // Day model rows summed per (provider, model): sessions can overlap
    // on a day, and stacked charts need one segment per model.
    std::map<std::string,
        std::map<std::pair<std::string, std::string>, UsageModelRow>>
        day_models;
    for (const SavedSession& saved : saved) {
        out.prompt += saved.usage.prompt;
        out.completion += saved.usage.completion;
        out.total += saved.usage.total;
        out.total_cost += saved.usage.cost;
        for (const UsageModelRow& row : saved.usage.by_model) {
            auto& agg      = out.by_model.emplace_back();
            agg.provider   = row.provider;
            agg.model      = row.model;
            agg.prompt     = row.prompt;
            agg.completion = row.completion;
            agg.total      = row.total;
            agg.cost       = row.cost;
        }
        // Index entries written before per-day buckets existed: fall back
        // to the save day so the chart still sees their usage. saved_at
        // is "%Y-%m-%d %H:%M:%S".
        const std::string fallback_day
            = saved.saved_at.size() >= 10 ? saved.saved_at.substr(0, 10) : "";
        if (!saved.usage.by_day.empty()) {
            for (const UsageDayRow& row : saved.usage.by_day) {
                auto& agg = by_day[row.day];
                agg.day   = row.day;
                agg.total += row.total;
                agg.cost += row.cost;
                for (const UsageModelRow& model : row.by_model) {
                    auto& segment
                        = day_models[row.day][{ model.provider, model.model }];
                    segment.provider = model.provider;
                    segment.model    = model.model;
                    segment.prompt += model.prompt;
                    segment.completion += model.completion;
                    segment.total += model.total;
                    segment.cost += model.cost;
                }
            }
        } else if (!fallback_day.empty()
            && (saved.usage.total > 0 || saved.usage.cost > 0.0)) {
            auto& agg = by_day[fallback_day];
            agg.day   = fallback_day;
            agg.total += saved.usage.total;
            agg.cost += saved.usage.cost;
            // Legacy bucket: the session's model rows are the best
            // attribution its usage has.
            for (const UsageModelRow& model : saved.usage.by_model) {
                auto& segment
                    = day_models[fallback_day][{ model.provider, model.model }];
                segment.provider = model.provider;
                segment.model    = model.model;
                segment.prompt += model.prompt;
                segment.completion += model.completion;
                segment.total += model.total;
                segment.cost += model.cost;
            }
        }
    }
    merge_rollup_rows(out.by_model);
    for (auto& [day, row] : by_day) {
        auto& rows = day_models[day];
        row.by_model.reserve(rows.size());
        for (auto& [key, model] : rows) {
            row.by_model.push_back(std::move(model));
        }
        out.by_day.push_back(std::move(row));
    }
    return out;
}

GlobalUsageSummary global_usage_summary()
{
    return summarize_saved_usage(saved_sessions());
}

bool session_file_locked(const std::filesystem::path& path)
{
    return foreign_lock_live(lock_path_for(path));
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
