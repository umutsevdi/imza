#include "acp/adapter.h"

#include "app/flows.h"
#include "app/slash_commands.h"
#include "conversation/persistence.h"
#include "conversation/tool_format.h"
#include "platform/config.h"
#include "platform/file_lock.h"
#include "providers/provider_store.h"

#include <algorithm>
#include <future>
#include <ranges>
#include <variant>

namespace imza::acp {

namespace {

    constexpr const char* PERMISSION_METHOD = "session/request_permission";
    constexpr const char* ELICIT_METHOD     = "elicitation/create";

    // flows' install_session is file-private; ACP sessions are child
    // states, which own no subagent manager, so pruning is guarded.
    void install_session(ApplicationState& state, SessionSnapshot snapshot)
    {
        state.session->restore(std::move(snapshot));
        state.permissions->clear();
        state.runner->clear();
        if (state.subagents != nullptr) {
            state.subagents->prune_completed();
        }
    }

    std::string mode_name(Session::Mode mode)
    {
        return mode == Session::Mode::PLAN ? "plan" : "build";
    }

    JsonValue object() { return JsonValue(JsonValue::object_t { }); }

    std::string string_member(const JsonValue& value, std::string_view key)
    {
        const JsonValue* found = find_member(value, key);
        if (found == nullptr || !found->is_string()) {
            return { };
        }
        return found->get<std::string>();
    }

    std::string join_choices(const SlashCommand& command)
    {
        std::string out;
        for (const std::string_view choice : command.argument_choices) {
            if (!out.empty()) {
                out += ", ";
            }
            out += choice;
        }
        return out;
    }

    // session/new | load | resume may widen the session's filesystem
    // scope with absolute extra roots. Each becomes a session grant, the
    // same thing an approved directory ask installs, so the permission
    // evaluator stays the single authorization point.
    void install_additional_directories(
        ApplicationState& state, const JsonValue& params)
    {
        const JsonValue* directories
            = find_member(params, "additionalDirectories");
        if (directories == nullptr || !directories->is_array()) {
            return;
        }
        PermissionStore::Grants grants;
        for (const JsonValue& entry : directories->get<JsonValue::array_t>()) {
            if (!entry.is_string()) {
                continue;
            }
            std::error_code error;
            const std::filesystem::path root
                = std::filesystem::weakly_canonical(
                    path_from_utf8(entry.get<std::string>()), error);
            if (error || root.empty()) {
                continue;
            }
            grants.push_back(PermissionGrant { root });
        }
        if (!grants.empty()) {
            state.permissions->install(std::move(grants));
        }
    }

    JsonValue object_of(
        std::initializer_list<std::pair<std::string, JsonValue>> entries)
    {
        JsonValue value = JsonValue(JsonValue::object_t { });
        auto& object    = value.get<JsonValue::object_t>();
        for (auto& [key, item] : entries) {
            object[key] = item;
        }
        return value;
    }

} // namespace

AcpInterface::AcpInterface(ApplicationState& root, Config config)
    : _root(root)
    , _config(std::move(config))
{
}

void AcpInterface::set_writer(WriteFn write)
{
    std::lock_guard lock(_mutex);
    _write = std::move(write);
}

void AcpInterface::reply(const JsonValue& id, const JsonValue& result)
{
    std::string line = dump_response(id, result);
    std::lock_guard lock(_mutex);
    if (_write) {
        _write(line);
    }
}

void AcpInterface::reply_error(const JsonValue& id, int code,
    std::string_view message, const std::string& session_id)
{
    std::string line;
    if (!session_id.empty()) {
        // -32002 carries data.sessionId per the spec.
        JsonValue error   = object();
        error["code"]     = static_cast<double>(code);
        error["message"]  = std::string(message);
        JsonValue data    = object();
        data["sessionId"] = session_id;
        error["data"]     = data;
        JsonValue frame;
        frame["jsonrpc"] = "2.0";
        frame["id"]      = id;
        frame["error"]   = error;
        line             = json_dump(frame);
    } else {
        line = dump_error(id, code, message);
    }
    std::lock_guard lock(_mutex);
    if (_write) {
        _write(line);
    }
}

AcpInterface::AcpSession* AcpInterface::find_session(const std::string& id)
{
    const auto found = _sessions.find(id);
    return found == _sessions.end() ? nullptr : &found->second;
}

void AcpInterface::handle_request(
    const JsonValue& id, std::string_view method, const JsonValue& params)
{
    const std::string name(method);
    if (name == "initialize") {
        handle_initialize(id, params);
        return;
    }
    if (!require_initialized(id, name)) {
        return;
    }
    if (name == "session/new") {
        if (accept_cwd(id, params)) {
            handle_session_new(id, params);
        }
    } else if (name == "session/load" || name == "session/resume") {
        if (accept_cwd(id, params)) {
            handle_session_load(id, name, params);
        }
    } else if (name == "session/prompt") {
        handle_session_prompt(id, params);
    } else if (name == "session/list") {
        handle_session_list(id, params);
    } else if (name == "session/delete") {
        handle_session_delete(id, params);
    } else if (name == "session/close") {
        handle_session_close(id, params);
    } else if (name == "session/set_config_option"
        || name == "session/set_mode") {
        handle_set_config_option(id, params, name);
    } else if (!name.empty() && name.front() == '_') {
        reply_error(id, ERR_METHOD_NOT_FOUND, "method not found");
    } else {
        reply_error(id, ERR_METHOD_NOT_FOUND, "method not found");
    }
}

void AcpInterface::handle_notification(
    std::string_view method, const JsonValue& params)
{
    const std::string name(method);
    if (name != "session/cancel") {
        return; // Unknown notifications are ignored per spec.
    }
    const std::string session_id = string_member(params, "sessionId");
    std::lock_guard lock(_mutex);
    AcpSession* session = find_session(session_id);
    if (session == nullptr) {
        return;
    }
    // The wire equivalent of the UI's close_modal: unblock the
    // waiting worker, then interrupt.
    if (session->awaiting_reply) {
        session->client_reply.set_value(JsonValue { });
        session->awaiting_reply = false;
    }
    session->state->session->request_interrupt();
}

JsonValue AcpInterface::call_client(
    std::string_view method, const JsonValue& params, bool& cancelled)
{
    std::int64_t call_id = 0;
    std::future<JsonValue> reply;
    const std::string session_id = string_member(params, "sessionId");
    {
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(session_id);
        if (session == nullptr) {
            cancelled = true;
            return object();
        }
        call_id                      = _next_client_call++;
        session->pending_client_call = call_id;
        session->client_reply        = std::promise<JsonValue> { };
        session->awaiting_reply      = true;
        reply                        = session->client_reply.get_future();
    }
    const std::string line = dump_request(call_id, method, params);
    {
        std::lock_guard lock(_mutex);
        if (_write) {
            _write(line);
        }
    }
    // Blocks the turn worker until the client answers or cancels.
    JsonValue result = reply.get();
    if (result.is_null()) {
        cancelled = true;
        return object();
    }
    return result;
}

void AcpInterface::handle_response(const Message& message)
{
    // Client responses answer our generated numeric ids only.
    if (!message.id.is_number()) {
        return;
    }
    const std::int64_t call_id
        = static_cast<std::int64_t>(message.id.get<double>());
    std::lock_guard lock(_mutex);
    for (auto& [id, session] : _sessions) {
        if (session.pending_client_call == call_id && session.awaiting_reply) {
            JsonValue result
                = message.result.has_value() ? *message.result : JsonValue { };
            session.client_reply.set_value(std::move(result));
            session.awaiting_reply      = false;
            session.pending_client_call = std::nullopt;
            return;
        }
    }
}

void AcpInterface::handle_initialize(
    const JsonValue& id, const JsonValue& params)
{
    const InitializeRequest request = InitializeRequest::parse(params);
    _elicitation_form               = request.client.elicitation_form;
    _elicitation_url                = request.client.elicitation_url;
    _compaction_capable             = request.client.compaction;
    _notices_capable                = request.client.notices;
    _boolean_options                = request.client.boolean_options;
    _initialized                    = true;
    if (!request.client_name.empty()) {
        _client_label = request.client_name;
    }
    reply(id, dump_initialize_result(request));
}

bool AcpInterface::require_initialized(
    const JsonValue& id, std::string_view method)
{
    if (_initialized) {
        return true;
    }
    reply_error(id, ERR_INVALID_REQUEST,
        "request received before initialization: " + std::string(method));
    return false;
}

void AcpInterface::send_available_commands(AcpSession& session)
{
    std::vector<std::tuple<std::string, std::string, std::string>> commands;
    for (const SlashCommand& command : slash_commands()) {
        // Every command is advertised; modal-only ones answer with a
        // notice when the client invokes them. Argument-taking commands
        // advertise their input hint.
        const std::string hint = command.takes_argument
            ? std::string(command.argument_choices.empty()
                      ? "arguments"
                      : "one of: " + join_choices(command))
            : "";
        commands.emplace_back(
            std::string(command.name), std::string(command.desc), hint);
        for (const std::string_view alias : command.aliases) {
            commands.emplace_back(
                std::string(alias), std::string(command.desc), hint);
        }
    }
    std::string line = session.updates.available_commands(commands);
    if (_write) {
        _write(line);
    }
}

std::vector<ConfigOption> AcpInterface::config_options_for(AcpSession& session)
{
    const std::optional<ProviderSelection> selection
        = session.state->providers->active_selection();
    // The same model list the TUI model picker offers, for the client's
    // selector; empty when no connection is active.
    std::vector<std::pair<std::string, std::string>> models;
    if (selection.has_value()) {
        const ModelList list
            = session.state->providers->models_for(selection->connection_id);
        if (list.state == ModelList::State::READY) {
            for (const ModelInfo& info : list.models) {
                models.emplace_back(
                    info.id, info.name.empty() ? info.id : info.name);
            }
        }
    }
    const std::optional<bool> skip = _boolean_options
        ? std::optional<bool>(session.state->permissions->skip_permissions())
        : std::nullopt;
    return config_options(mode_name(session.state->session->mode()),
        selection ? selection->model : "",
        selection ? to_config_effort(selection->reasoning_effort) : "default",
        models, skip);
}

void AcpInterface::send_config_options(AcpSession& session)
{
    std::string line
        = session.updates.config_option_update(config_options_for(session));
    if (_write) {
        _write(line);
    }
}

void AcpInterface::handle_session_new(
    const JsonValue& id, const JsonValue& params)
{
    // Client-supplied mcpServers are ignored: imza owns its MCP stack.
    // The protocol id is imza's session id, which becomes the saved
    // file stem, so the client can load the conversation after a restart.
    std::string protocol_id;
    std::shared_ptr<ApplicationState> state;
    {
        // The child is built before its id exists; routing resolves
        // the entry at call time.
        auto routing       = std::make_shared<std::string>();
        StreamFn inherited = _root.runner->has_stream_override()
            ? _root.runner->stream_fn()
            : StreamFn { };
        state              = make_child_application_state(
            _root, _root.post, std::move(inherited),
            [this, routing](ModalPayload payload) {
                std::lock_guard lock(_mutex);
                AcpSession* session = find_session(*routing);
                if (session == nullptr) {
                    std::promise<ModalResult> dead;
                    dead.set_value(std::monostate { });
                    return dead.get_future();
                }
                return request_modal(*session, std::move(payload));
            },
            "ACP");
        SessionSnapshot snapshot;
        snapshot.client      = _client_label;
        snapshot.persistence = UnsavedSession { };
        state->session->restore(std::move(snapshot));
        install_additional_directories(*state, params);
        protocol_id = state->session->session_id();
        *routing    = protocol_id;
    }

    {
        std::lock_guard lock(_mutex);
        auto [entry, inserted] = _sessions.try_emplace(
            protocol_id, protocol_id, state, UpdateBuilder(protocol_id));
        if (!inserted) {
            reply_error(id, ERR_INTERNAL_ERROR, "session id collision");
            return;
        }
        install_observer(entry->second);
    }
    // Carried in the reply so the client renders its switchers at once.
    JsonValue result = object_of({ { "sessionId", JsonValue(protocol_id) } });
    {
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(protocol_id);
        if (session != nullptr) {
            result["configOptions"]
                = dump_config_options(config_options_for(*session));
            result["modes"] = dump_session_modes(
                mode_name(session->state->session->mode()));
        }
    }
    reply(id, result);
    {
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(protocol_id);
        if (session != nullptr) {
            send_available_commands(*session);
        }
    }
}

void AcpInterface::handle_session_load(
    const JsonValue& id, std::string_view method, const JsonValue& params)
{
    const std::string requested     = string_member(params, "sessionId");
    std::vector<SavedSession> saved = saved_sessions_of(_client_label);
    const auto found                = std::find_if(
        saved.begin(), saved.end(), [&](const SavedSession& entry) {
            return entry.path.stem().string() == requested;
        });
    if (found == saved.end()) {
        reply_error(id, ERR_RESOURCE_NOT_FOUND, "session not found", requested);
        return;
    }
    // The chat-active guard: a second writer (TUI or another ACP
    // connection) is refused instead of racing this one on the same
    // file. Probed before read_session, whose own foreign-lock check
    // would otherwise report the generic "session not found".
    if (foreign_lock_live(lock_path_for(found->path))) {
        reply_error(id, ERR_RESOURCE_NOT_FOUND,
            "session is open in another imza process", requested);
        return;
    }
    LoadedSession loaded;
    if (read_session(found->path, loaded) != Status::OK) {
        reply_error(id, ERR_RESOURCE_NOT_FOUND, "session not found", requested);
        return;
    }
    auto guard = acquire_file_lock(lock_path_for(found->path),
        FileLockRequest { FileLockMode::EXCLUSIVE, false });
    if (!std::holds_alternative<FileLock>(guard)) {
        reply_error(id, ERR_RESOURCE_NOT_FOUND,
            "session is open in another imza process", requested);
        return;
    }
    // The requested id is the saved file stem, which restore adopts
    // as the session's own id.
    const std::string protocol_id = requested;
    StreamFn inherited            = _root.runner->has_stream_override()
        ? _root.runner->stream_fn()
        : StreamFn { };
    auto state                    = make_child_application_state(
        _root, _root.post, std::move(inherited),
        [this, protocol_id](ModalPayload payload) {
            std::lock_guard lock(_mutex);
            AcpSession* session = find_session(protocol_id);
            if (session == nullptr) {
                std::promise<ModalResult> dead;
                dead.set_value(std::monostate { });
                return dead.get_future();
            }
            return request_modal(*session, std::move(payload));
        },
        "ACP");
    install_session(*state, std::move(loaded.snapshot));
    install_additional_directories(*state, params);

    {
        std::lock_guard lock(_mutex);
        auto [entry, inserted] = _sessions.try_emplace(
            protocol_id, protocol_id, state, UpdateBuilder(protocol_id));
        if (!inserted) {
            reply_error(id, ERR_INTERNAL_ERROR, "session id collision");
            return;
        }
        entry->second.lock = std::move(std::get<FileLock>(guard));
        install_observer(entry->second);
    }
    if (method != "session/resume") {
        // Replay precedes the response, per spec.
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(protocol_id);
        if (session == nullptr) {
            return;
        }
        const auto items        = session->state->session->items();
        std::int64_t message_id = _next_client_call;
        std::vector<std::string> frames;
        for (const ConversationItem& item : *items) {
            if (const auto* user = std::get_if<UserTurn>(&item)) {
                JsonValue update  = update_of("user_message_chunk");
                update["content"] = text_block(user->text);
                frames.push_back(dump_notification("session/update",
                    session_update(protocol_id, std::move(update))));
            } else if (const auto* assistant
                = std::get_if<AssistantTurn>(&item)) {
                if (!assistant->reasoning.empty()) {
                    frames.push_back(session->updates.agent_thought(
                        assistant->reasoning, message_id));
                }
                if (!assistant->markdown.empty()) {
                    frames.push_back(session->updates.agent_message(
                        assistant->markdown, message_id));
                }
                ++message_id;
            } else if (const auto* tool = std::get_if<ToolCall>(&item)) {
                frames.push_back(session->updates.tool_call(*tool));
            } else if (const auto* todo = std::get_if<TodoList>(&item)) {
                frames.push_back(session->updates.plan(*todo));
            } else if (const auto* compaction
                = std::get_if<CompactionEvent>(&item);
                compaction != nullptr
                && compaction->status == CompactionEvent::Status::COMPLETED) {
                frames.push_back(session->updates.compaction_completed(
                    static_cast<std::int64_t>(compaction->id), ""));
            }
        }
        if (!session->state->session->title().empty()) {
            frames.push_back(session->updates.session_info(
                session->state->session->title()));
        }
        for (const std::string& frame : frames) {
            if (_write) {
                _write(frame);
            }
        }
    }
    JsonValue result = object();
    {
        std::lock_guard lock(_mutex);
        if (AcpSession* session = find_session(protocol_id);
            session != nullptr) {
            result["configOptions"]
                = dump_config_options(config_options_for(*session));
            result["modes"] = dump_session_modes(
                mode_name(session->state->session->mode()));
        }
    }
    reply(id, result);
}

void AcpInterface::handle_session_prompt(
    const JsonValue& id, const JsonValue& params)
{
    const PromptRequest request = PromptRequest::parse(params);
    std::shared_ptr<ApplicationState> state;
    {
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(request.session_id);
        if (session == nullptr) {
            state = nullptr;
        } else {
            state = _sessions.find(request.session_id)->second.state;
        }
    }
    if (state == nullptr) {
        reply_error(
            id, ERR_RESOURCE_NOT_FOUND, "unknown session", request.session_id);
        return;
    }
    std::string text;
    std::vector<Attachment> attachments;
    for (const PromptContent& block : request.prompt) {
        switch (block.kind) {
        case PromptContent::Kind::TEXT: {
            if (!text.empty()) {
                text += "\n\n";
            }
            text += block.text;
            break;
        }
        case PromptContent::Kind::IMAGE: {
            Attachment image;
            image.type    = Attachment::Type::IMAGE;
            image.encoded = block.data;
            image.media_type
                = block.media_type.empty() ? "image/png" : block.media_type;
            attachments.push_back(std::move(image));
            break;
        }
        case PromptContent::Kind::RESOURCE: {
            // Text resources ride as text attachments; blob resources as
            // media. The uri names the source for the model.
            if (!block.data.empty()) {
                Attachment blob;
                blob.type       = Attachment::Type::IMAGE;
                blob.encoded    = block.data;
                blob.media_type = block.media_type.empty()
                    ? "application/octet-stream"
                    : block.media_type;
                blob.path       = block.uri;
                attachments.push_back(std::move(blob));
            } else if (!block.text.empty()) {
                Attachment file;
                file.type    = Attachment::Type::TEXT;
                file.content = block.text;
                file.path    = block.uri;
                attachments.push_back(std::move(file));
            } else {
                if (!text.empty()) {
                    text += "\n\n";
                }
                text += block.uri;
            }
            break;
        }
        case PromptContent::Kind::RESOURCE_LINK: {
            // A reference, not content: name it in the text and let the
            // agent read it through its own file access.
            if (!text.empty()) {
                text += "\n\n";
            }
            text += block.name.empty() ? block.uri
                                       : block.name + " (" + block.uri + ")";
            break;
        }
        case PromptContent::Kind::OTHER: break;
        }
    }
    // Own thread: the wire loop must keep reading, or cancel and a
    // second session's frames queue behind this prompt forever.
    std::lock_guard lock(_mutex);
    AcpSession* session = find_session(request.session_id);
    if (session == nullptr) {
        return;
    }
    session->prompt_running.store(true, std::memory_order_release);
    const std::string session_id = request.session_id;
    _prompt_threads.emplace_back(
        [this, id, session_id, state, text = std::move(text),
            attachments = std::move(attachments)]() mutable {
            run_prompt_turn(
                id, session_id, state, std::move(text), std::move(attachments));
        });
}

void AcpInterface::run_prompt_turn(const JsonValue& id,
    const std::string& session_id, std::shared_ptr<ApplicationState> state,
    std::string text, std::vector<Attachment> attachments)
{
    submit(*state, std::move(text), std::move(attachments));

    MainThreadQueue& main_thread = *_root.main_thread;
    while (state->session->has_pending_work() && _root.alive.load()) {
        main_thread.wait_for_task(std::chrono::milliseconds(50));
        main_thread.drain();
    }
    main_thread.drain();

    StopReason reason = StopReason::END_TURN;
    if (state->session->interrupt_requested()) {
        reason = StopReason::CANCELLED;
    } else if (state->runner->blocked_permission()) {
        reason = StopReason::REFUSAL;
    } else if (const std::string error = state->session->error();
        !error.empty()) {
        // A response cut off by the token limit is a distinct stop
        // reason; every other failure is a refusal.
        reason = state->runner->last_fail() == Status::TRUNCATED
            ? StopReason::MAX_TOKENS
            : StopReason::REFUSAL;
    }
    // Persist every settled turn so list/load see it without a clean
    // exit.
    if (state->session->has_items()
        && save_session(*state->session) == Status::OK) {
        // A brand-new session's file only exists after its first save;
        // guard it from that point on. save_session names the file after
        // the session id, so the path is derived, not re-read.
        const std::filesystem::path path
            = sessions_dir() / (state->session->session_id() + ".json");
        std::lock_guard lock(_mutex);
        if (AcpSession* session = find_session(session_id);
            session != nullptr && !session->lock.has_value()) {
            auto guard = acquire_file_lock(lock_path_for(path),
                FileLockRequest { FileLockMode::EXCLUSIVE, false });
            if (std::holds_alternative<FileLock>(guard)) {
                session->lock = std::move(std::get<FileLock>(guard));
            }
        }
    }
    JsonValue result     = object();
    result["stopReason"] = std::string(stop_reason_name(reason));
    reply(id, result);

    std::lock_guard lock(_mutex);
    if (AcpSession* session = find_session(session_id); session != nullptr) {
        session->prompt_running.store(false, std::memory_order_release);
        if (session->close_requested) {
            _sessions.erase(session_id); // drops the lock with the entry
        }
    }
}

bool AcpInterface::accept_cwd(const JsonValue& id, const JsonValue& params)
{
    const std::string requested = string_member(params, "cwd");
    if (requested.empty()) {
        return true;
    }
    std::error_code error;
    const std::filesystem::path given
        = std::filesystem::weakly_canonical(path_from_utf8(requested), error);
    std::error_code cwd_error;
    const std::filesystem::path current = std::filesystem::weakly_canonical(
        std::filesystem::current_path(cwd_error), error);
    if (error || cwd_error || given != current) {
        reply_error(id, ERR_INVALID_PARAMS,
            "cwd must be the directory imza was started in: "
                + portable_string(current));
        return false;
    }
    return true;
}

void AcpInterface::handle_session_list(
    const JsonValue& id, const JsonValue& params)
{
    std::vector<SavedSession> saved = saved_sessions_of(_client_label);
    const std::string requested_cwd = string_member(params, "cwd");
    if (!requested_cwd.empty()) {
        std::error_code error;
        const std::filesystem::path filter = std::filesystem::weakly_canonical(
            path_from_utf8(requested_cwd), error);
        if (error || !filter.is_absolute()) {
            reply_error(id, ERR_INVALID_PARAMS, "cwd must be an absolute path");
            return;
        }
        std::erase_if(saved, [&filter](const SavedSession& entry) {
            if (!entry.workspace.has_value()) {
                return true;
            }
            std::error_code entry_error;
            const std::filesystem::path canonical
                = std::filesystem::weakly_canonical(
                    *entry.workspace, entry_error);
            return entry_error || canonical != filter;
        });
    }
    // Opaque offset cursor; the listing is sorted, so it is stable.
    constexpr std::size_t PAGE = 100;
    std::size_t offset         = 0;
    const std::string cursor   = string_member(params, "cursor");
    if (!cursor.empty()) {
        try {
            offset = static_cast<std::size_t>(std::stoull(cursor));
        } catch (const std::exception&) {
            reply_error(id, ERR_INVALID_PARAMS, "invalid cursor");
            return;
        }
    }
    JsonValue result = object();
    JsonValue::array_t entries;
    const std::size_t end = std::min(saved.size(), offset + PAGE);
    for (std::size_t index = offset; index < end; ++index) {
        const SavedSession& entry = saved[index];
        JsonValue item;
        item["sessionId"] = entry.path.stem().string();
        item["title"]     = entry.title;
        item["updatedAt"] = rfc3339_from_local(entry.saved_at);
        // Required and absolute: clients group imported threads by it.
        item["cwd"] = entry.workspace
            ? portable_string(*entry.workspace)
            : portable_string(std::filesystem::current_path());
        entries.push_back(std::move(item));
    }
    result["sessions"] = std::move(entries);
    if (end < saved.size()) {
        result["nextCursor"] = std::to_string(end);
    }
    reply(id, result);
}

void AcpInterface::handle_session_delete(
    const JsonValue& id, const JsonValue& params)
{
    const std::string session_id    = string_member(params, "sessionId");
    std::vector<SavedSession> saved = saved_sessions_of(_client_label);
    const auto found                = std::find_if(
        saved.begin(), saved.end(), [&](const SavedSession& entry) {
            return entry.path.stem().string() == session_id;
        });
    if (found == saved.end()) {
        // Deleting an unknown or already-deleted session succeeds
        // silently; clients treat -32002 as success either way.
        reply(id, object());
        return;
    }
    {
        // Drop this process's own guard first, or the delete would fail
        // on a lock we hold ourselves.
        std::lock_guard lock(_mutex);
        if (AcpSession* session = find_session(session_id);
            session != nullptr) {
            session->lock.reset();
        }
    }
    switch (delete_saved_session(found->path)) {
    case DeleteSessionResult::OK: reply(id, object()); return;
    case DeleteSessionResult::INVALID_PATH:
        reply_error(id, ERR_INVALID_PARAMS, "invalid session path", session_id);
        return;
    case DeleteSessionResult::REMOVE_FAILED:
        reply_error(id, ERR_INTERNAL_ERROR,
            "session is open in another imza process", session_id);
        return;
    }
    reply(id, object());
}

void AcpInterface::handle_session_close(
    const JsonValue& id, const JsonValue& params)
{
    const std::string session_id = string_member(params, "sessionId");
    {
        std::lock_guard lock(_mutex);
        // Idempotent. A running prompt erases its own entry when the
        // turn settles, so close only interrupts it.
        if (AcpSession* session = find_session(session_id);
            session != nullptr) {
            if (session->prompt_running.load(std::memory_order_acquire)) {
                session->close_requested = true;
                session->state->session->request_interrupt();
            } else {
                session->lock.reset(); // release the chat-active guard
                _sessions.erase(session_id);
            }
        }
    }
    reply(id, object());
}

void AcpInterface::handle_set_config_option(
    const JsonValue& id, const JsonValue& params, std::string_view method)
{
    const std::string session_id = string_member(params, "sessionId");
    // The legacy session/set_mode method names the target differently from
    // session/set_config_option; both reach the same switch.
    const bool legacy_mode = method == "session/set_mode";
    const std::string config_id
        = legacy_mode ? "mode" : string_member(params, "configId");
    const JsonValue* value = find_member(params, "value");
    std::string typed      = value != nullptr && value->is_string()
        ? value->get<std::string>()
        : "";
    bool typed_bool        = false;
    if (value != nullptr && value->is_boolean()) {
        typed_bool = value->get<bool>();
        typed      = typed_bool ? "true" : "false";
    }
    if (legacy_mode) {
        typed = string_member(params, "modeId");
    }
    const bool switching_mode
        = config_id == "mode" && (typed == "plan" || typed == "build");
    // Malformed parameters and unsupported values get -32602, per the
    // error page (unknown configId or value). Validated before any
    // adapter-mutex scope: reply_error takes the same mutex.
    const bool known = switching_mode || config_id == "thought_level"
        || config_id == "model" || config_id == "skip_permissions";
    if (!known) {
        reply_error(id, ERR_INVALID_PARAMS, "unknown configId: " + config_id,
            session_id);
        return;
    }
    if (config_id == "thought_level" && !typed.empty()
        && !std::ranges::contains(REASONING_EFFORT_CHOICES, typed)) {
        reply_error(
            id, ERR_INVALID_PARAMS, "unknown value: " + typed, session_id);
        return;
    }
    std::shared_ptr<ApplicationState> mode_target;
    {
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(session_id);
        if (session == nullptr) {
            reply_error(
                id, ERR_RESOURCE_NOT_FOUND, "unknown session", session_id);
            return;
        }
        if (switching_mode) {
            mode_target = session->state;
        } else if (config_id == "thought_level" && !typed.empty()) {
            session->state->providers->set_reasoning_effort(
                to_wire_effort(typed));
        } else if (config_id == "model" && !typed.empty()) {
            if (const auto selection
                = session->state->providers->active_selection();
                selection != std::nullopt) {
                session->state->providers->select_model(
                    { selection->connection_id, typed });
            }
        } else if (config_id == "skip_permissions") {
            session->state->permissions->set_skip_permissions(typed_bool);
        }
    }
    // set_mode publishes synchronously on this thread and the observer
    // re-locks _mutex, so it must run outside the lock.
    if (mode_target != nullptr) {
        mode_target->session->set_mode(
            typed == "plan" ? Session::Mode::PLAN : Session::Mode::BUILD);
    }
    JsonValue result = object();
    // The reply carries the complete new state; the notification
    // pushes it to the client's switchers.
    {
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(session_id);
        if (session != nullptr) {
            result["configOptions"]
                = dump_config_options(config_options_for(*session));
            send_config_options(*session);
        }
    }
    reply(id, result);
}

void AcpInterface::send_usage_update_for(const std::string& session_id)
{
    std::string frame;
    {
        std::lock_guard lock(_mutex);
        AcpSession* current = find_session(session_id);
        if (current == nullptr) {
            return;
        }
        const Session::StatusView view = current->state->session->status_view();
        std::uint64_t window           = 0;
        if (const auto selection
            = current->state->providers->active_selection();
            selection != std::nullopt) {
            window = current->state->providers->context_limit_for(
                selection->model, selection->connection_id);
        }
        const std::uint64_t used = view.totals.total;
        if (window == 0 && used == 0) {
            return;
        }
        frame = current->updates.usage_update(used, window, view.total_cost);
    }
    std::lock_guard lock(_mutex);
    if (_write) {
        _write(frame);
    }
}

void AcpInterface::send_notice_for(const std::string& session_id,
    const char* severity, const std::string& message)
{
    std::string frame;
    {
        std::lock_guard lock(_mutex);
        AcpSession* current = find_session(session_id);
        if (current == nullptr) {
            return;
        }
        frame = current->updates.notice(severity, "Imza", message);
    }
    std::lock_guard lock(_mutex);
    if (_write) {
        _write(frame);
    }
}

void AcpInterface::install_observer(AcpSession& session)
{
    // Mode and title changes fire on whichever thread made them; the adapter
    // mutex is taken only to format and write the frame.
    session.mode_subscription
        = session.state->session->subscribe_to_mode_change(
            [this, id = session.id] {
                std::lock_guard lock(_mutex);
                AcpSession* current = find_session(id);
                if (current == nullptr || !_write) {
                    return;
                }
                _write(current->updates.current_mode(
                    mode_name(current->state->session->mode())));
            });
    session.title_subscription
        = session.state->session->subscribe_to_title_change(
            [this, id = session.id] {
                std::lock_guard lock(_mutex);
                AcpSession* current = find_session(id);
                if (current == nullptr || !_write) {
                    return;
                }
                const std::string title = current->state->session->title();
                if (!title.empty()) {
                    _write(current->updates.session_info(title));
                }
            });
    // The todo list lives outside the transcript, so the client would
    // never see plan changes without this push.
    session.todo_subscription
        = session.state->session->subscribe_to_todo_change(
            [this, id = session.id] {
                std::string frame;
                {
                    std::lock_guard lock(_mutex);
                    AcpSession* current = find_session(id);
                    if (current == nullptr || !_write) {
                        return;
                    }
                    frame = current->updates.plan(
                        *current->state->session->todo());
                }
                std::lock_guard lock(_mutex);
                if (_write) {
                    _write(frame);
                }
            });
    session.compaction_subscription
        = session.state->session->subscribe_to_compaction_change(
            [this, id = session.id] {
                // Re-snapshot: the event that fired may not be the newest.
                std::optional<CompactionEvent> event;
                std::string summary;
                {
                    std::lock_guard lock(_mutex);
                    AcpSession* current = find_session(id);
                    if (current == nullptr) {
                        return;
                    }
                    // Compaction updates are capability-gated.
                    if (!_compaction_capable) {
                        return;
                    }
                    const auto items = current->state->session->items();
                    for (const ConversationItem& item :
                        *items | std::views::reverse) {
                        if (const auto* entry
                            = std::get_if<CompactionEvent>(&item)) {
                            event = *entry;
                            break;
                        }
                    }
                    summary = current->state->session->compacted_summary();
                }
                if (!event.has_value()) {
                    return;
                }
                std::optional<std::string> frame;
                {
                    std::lock_guard lock(_mutex);
                    AcpSession* current = find_session(id);
                    if (current == nullptr || !_write) {
                        return;
                    }
                    switch (event->status) {
                    case CompactionEvent::Status::RUNNING:
                        frame = current->updates.compaction_in_progress(
                            static_cast<std::int64_t>(event->id));
                        break;
                    case CompactionEvent::Status::COMPLETED:
                        frame = current->updates.compaction_completed(
                            static_cast<std::int64_t>(event->id), summary);
                        break;
                    case CompactionEvent::Status::FAILED:
                        frame = current->updates.compaction_failed(
                            static_cast<std::int64_t>(event->id), "");
                        break;
                    }
                }
                if (frame.has_value()) {
                    std::lock_guard lock(_mutex);
                    if (_write) {
                        _write(*frame);
                    }
                }
            });
    session.usage_subscription
        = session.state->session->subscribe_to_usage_change(
            [this, id = session.id] { send_usage_update_for(id); });
    session.notice_subscription = session.state->session->subscribe_to_notice(
        [this, id = session.id](const std::string& message) {
            if (!_notices_capable) {
                return;
            }
            send_notice_for(id, "info", message);
        });
    session.error_subscription = session.state->session->subscribe_to_error(
        [this, id = session.id](const std::string& message) {
            if (!_notices_capable) {
                return;
            }
            send_notice_for(id, "error", message);
        });

    session.state->session->set_stream_observer(
        [this, id = session.id](const StreamEvent& event) {
            std::optional<std::string> frame;
            {
                std::lock_guard lock(_mutex);
                AcpSession* current = find_session(id);
                if (current == nullptr) {
                    return;
                }
                frame = stream_frame(*current, event);
            }
            if (frame.has_value()) {
                std::lock_guard lock(_mutex);
                if (_write) {
                    _write(*frame);
                }
            }
        });
    // Tool results land outside the stream.
    session.state->session->set_tool_result_observer(
        [this, id = session.id](const ToolCall& call) {
            std::string frame;
            {
                std::lock_guard lock(_mutex);
                AcpSession* current = find_session(id);
                if (current == nullptr) {
                    return;
                }
                frame = current->updates.tool_call_update_with_locations(call);
            }
            {
                std::lock_guard lock(_mutex);
                if (_write) {
                    _write(frame);
                }
            }
        });
}

std::optional<std::string> AcpInterface::stream_frame(
    AcpSession& session, const StreamEvent& event)
{
    switch (event.kind) {
    case StreamEvent::Kind::CONTENT_DELTA:
        // One id per provider request; a changed id marks a new message.
        return session.updates.agent_message(event.text, session.message_id);
    case StreamEvent::Kind::REASONING:
        return session.updates.agent_thought(event.text, session.message_id);
    case StreamEvent::Kind::TOOL_CALL: {
        // Args are complete; TOOL_CALL_START is skipped because its
        // args are still streaming.
        imza::ToolCall snapshot;
        snapshot.call_id = event.tool_call.id;
        snapshot.name    = event.tool_call.name;
        snapshot.args    = event.tool_call.args;
        snapshot.phase   = imza::ToolCall::Phase::EXECUTING;
        ++session.message_id;
        return session.updates.tool_call(snapshot);
    }
    case StreamEvent::Kind::DONE:
    case StreamEvent::Kind::ERROR:
    case StreamEvent::Kind::USAGE:
    case StreamEvent::Kind::CONNECTED: return std::nullopt;
    }
    return std::nullopt;
}

std::future<ModalResult> AcpInterface::request_modal(
    AcpSession& session, ModalPayload payload)
{
    std::promise<ModalResult> done;
    auto future = done.get_future();
    // The turn worker blocks in call_client, as it blocks on a modal
    // future under the TUI.
    if (auto* permission = std::get_if<PermissionPrompt>(&payload)) {
        const auto request = permission_request_for(session.id, *permission);
        if (!request.has_value()) {
            done.set_value(ToolVerdict {
                ToolDecision::REJECT, "permission presentation unavailable" });
            return future;
        }
        JsonValue params    = object();
        params["sessionId"] = session.id;
        params["toolCall"]  = request->tool_call_update;
        JsonValue::array_t options;
        for (const PermissionOption& option : request->options) {
            JsonValue entry;
            entry["optionId"] = option.id;
            entry["name"]     = option.name;
            entry["kind"]     = option.kind;
            options.push_back(std::move(entry));
        }
        params["options"]   = std::move(options);
        bool cancelled      = false;
        const JsonValue raw = call_client(PERMISSION_METHOD, params, cancelled);
        const std::string outcome
            = cancelled ? "" : permission_outcome_kind(raw);
        if (cancelled || outcome.empty()) {
            done.set_value(std::monostate { });
            return future;
        }
        if (outcome == "allow_once") {
            done.set_value(ToolVerdict { ToolDecision::ACCEPT_ONCE, "" });
            return future;
        }
        if (outcome == "allow_always") {
            // The runner worker installs the session grant.
            done.set_value(
                ToolVerdict { ToolDecision::ACCEPT_FOR_SESSION, "" });
            return future;
        }
        std::string reason;
        if (const JsonValue* meta = find_member(raw, "_meta");
            meta != nullptr && meta->is_object()) {
            reason = string_member(*meta, "_imza.reject_reason");
        }
        if (permission_outcome_rejects_always(raw) && reason.empty()) {
            // No deny-store exists yet: the rejection applies to this
            // call; the reason tells the model the user asked for more.
            reason = "rejected for this session";
        }
        done.set_value(ToolVerdict { ToolDecision::REJECT, reason });
        return future;
    }
    if (auto* form = std::get_if<QuestionForm>(&payload)) {
        if (!_elicitation_form) {
            done.set_value(std::monostate { });
            return future;
        }
        JsonValue params    = object();
        params["sessionId"] = session.id;
        params["mode"]      = "form";
        params["message"]   = form->front().prompt;
        JsonValue::object_t properties;
        for (std::size_t index = 0; index < form->size(); ++index) {
            const QuestionCard& card = (*form)[index];
            JsonValue field;
            field["type"]        = "string";
            field["description"] = card.prompt;
            if (!card.options.empty()) {
                field["enum"] = JsonValue(JsonValue::array_t(
                    std::begin(card.options), std::end(card.options)));
            }
            properties["card" + std::to_string(index + 1)] = std::move(field);
        }
        JsonValue schema          = object();
        schema["type"]            = "object";
        schema["properties"]      = std::move(properties);
        params["requestedSchema"] = std::move(schema);
        bool cancelled            = false;
        const JsonValue raw = call_client(ELICIT_METHOD, params, cancelled);
        if (cancelled) {
            done.set_value(std::monostate { });
            return future;
        }
        const JsonValue* action = find_member(raw, "action");
        const std::string value = action != nullptr && action->is_string()
            ? action->get<std::string>()
            : "cancel";
        if (value != "accept") {
            done.set_value(std::monostate { });
            return future;
        }
        ModalAnswer answer;
        const JsonValue* content = find_member(raw, "content");
        // Accepted form content is one object keyed by the requested
        // schema's property names ("card1", "card2", ...); a legacy
        // client's array-of-text-blocks is tolerated too.
        std::map<std::string, std::string> fields;
        if (content != nullptr && content->is_object()) {
            for (const auto& [key, value] :
                content->get<JsonValue::object_t>()) {
                if (value.is_string()) {
                    fields[key] = value.get<std::string>();
                } else if (value.is_array()) {
                    // Multi-select: an array of strings.
                    std::string joined;
                    for (const JsonValue& item :
                        value.get<JsonValue::array_t>()) {
                        if (!item.is_string()) {
                            continue;
                        }
                        if (!joined.empty()) {
                            joined += ", ";
                        }
                        joined += item.get<std::string>();
                    }
                    fields[key] = joined;
                }
            }
        } else if (content != nullptr && content->is_array()) {
            std::size_t index = 1;
            for (const JsonValue& block : content->get<JsonValue::array_t>()) {
                if (string_member(block, "type") == "text") {
                    fields["card" + std::to_string(index)]
                        = string_member(block, "text");
                }
                ++index;
            }
        }
        for (std::size_t index = 0; index < form->size(); ++index) {
            QuestionAnswer entry;
            entry.prompt          = (*form)[index].prompt;
            const std::string key = "card" + std::to_string(index + 1);
            if (const auto found = fields.find(key);
                found != fields.end() && !found->second.empty()) {
                if ((*form)[index].free_text) {
                    entry.free_text = found->second;
                } else {
                    std::size_t start = 0;
                    for (;;) {
                        const std::size_t comma
                            = found->second.find(", ", start);
                        entry.selected.push_back(found->second.substr(start,
                            comma == std::string::npos ? std::string::npos
                                                       : comma - start));
                        if (comma == std::string::npos) {
                            break;
                        }
                        start = comma + 2;
                    }
                }
            }
            answer.cards.push_back(std::move(entry));
        }
        done.set_value(std::move(answer));
        return future;
    }
    // No protocol surface for these modals: fail closed.
    done.set_value(std::monostate { });
    return future;
}

void AcpInterface::stop()
{
    mark_dead();
    // Interrupt running turns so their threads settle, then join.
    {
        std::lock_guard lock(_mutex);
        for (auto& [id, session] : _sessions) {
            if (session.prompt_running.load(std::memory_order_acquire)) {
                session.state->session->request_interrupt();
            }
            if (session.awaiting_reply) {
                session.client_reply.set_value(JsonValue { });
                session.awaiting_reply = false;
            }
        }
    }
    for (std::jthread& thread : _prompt_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    _prompt_threads.clear();
    // Erasing releases each session's chat-active lock; without this,
    // an editor-driven process exit would leave sessions looking locked.
    std::lock_guard lock(_mutex);
    _sessions.clear();
}

} // namespace imza::acp
