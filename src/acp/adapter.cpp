#include "acp/adapter.h"

#include "app/flows.h"
#include "app/slash_commands.h"
#include "conversation/persistence.h"
#include "conversation/tool_format.h"
#include "providers/provider_store.h"

#include <algorithm>
#include <future>
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
    } else if (name == "session/new") {
        handle_session_new(id, params);
    } else if (name == "session/load" || name == "session/resume") {
        handle_session_load(id, name, params);
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
        handle_set_config_option(id, params);
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
    reply(id, dump_initialize_result(request));
}

void AcpInterface::send_available_commands(AcpSession& session)
{
    std::vector<std::pair<std::string, std::string>> commands;
    for (const SlashCommand& command : slash_commands()) {
        // Every command is advertised; modal-only ones answer with a
        // notice when the client invokes them.
        commands.emplace_back(
            std::string(command.name), std::string(command.desc));
        for (const std::string_view alias : command.aliases) {
            commands.emplace_back(
                std::string(alias), std::string(command.desc));
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
    return config_options(mode_name(session.state->session->mode()),
        selection ? selection->model : "",
        selection ? to_config_effort(selection->reasoning_effort) : "default");
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
    (void)params;
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
        snapshot.session_type = SessionType::ACP;
        snapshot.persistence  = UnsavedSession { };
        state->session->restore(std::move(snapshot));
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
    std::vector<SavedSession> saved = saved_sessions_of(SessionType::ACP);
    const auto found                = std::find_if(
        saved.begin(), saved.end(), [&](const SavedSession& entry) {
            return entry.path.stem().string() == requested;
        });
    if (found == saved.end()) {
        reply_error(id, ERR_RESOURCE_NOT_FOUND, "session not found", requested);
        return;
    }
    LoadedSession loaded;
    if (read_session(found->path, loaded) != Status::OK) {
        reply_error(id, ERR_RESOURCE_NOT_FOUND, "session not found", requested);
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
    reply(id, object());
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
        case PromptContent::Kind::RESOURCE:
        case PromptContent::Kind::RESOURCE_LINK:
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
        reason = StopReason::REFUSAL;
    }
    // Persist every settled turn so list/load see it without a clean
    // exit.
    if (state->session->has_items()) {
        (void)save_session(*state->session);
    }
    JsonValue result     = object();
    result["stopReason"] = std::string(stop_reason_name(reason));
    reply(id, result);

    std::lock_guard lock(_mutex);
    if (AcpSession* session = find_session(session_id); session != nullptr) {
        session->prompt_running.store(false, std::memory_order_release);
        if (session->close_requested) {
            _sessions.erase(session_id);
        }
    }
}

void AcpInterface::handle_session_list(
    const JsonValue& id, const JsonValue& params)
{
    (void)params;
    JsonValue result = object();
    JsonValue::array_t entries;
    for (const SavedSession& saved : saved_sessions_of(SessionType::ACP)) {
        JsonValue entry;
        entry["sessionId"] = saved.path.stem().string();
        entry["title"]     = saved.title;
        entry["updatedAt"] = saved.saved_at;
        entry["cwd"]       = "";
        entries.push_back(std::move(entry));
    }
    result["sessions"] = std::move(entries);
    reply(id, result);
}

void AcpInterface::handle_session_delete(
    const JsonValue& id, const JsonValue& params)
{
    const std::string session_id    = string_member(params, "sessionId");
    std::vector<SavedSession> saved = saved_sessions_of(SessionType::ACP);
    const auto found                = std::find_if(
        saved.begin(), saved.end(), [&](const SavedSession& entry) {
            return entry.path.stem().string() == session_id;
        });
    if (found != saved.end()) {
        delete_saved_session(found->path);
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
                _sessions.erase(session_id);
            }
        }
    }
    reply(id, object());
}

void AcpInterface::handle_set_config_option(
    const JsonValue& id, const JsonValue& params)
{
    const std::string session_id = string_member(params, "sessionId");
    const std::string config_id  = string_member(params, "configId");
    const JsonValue* value       = find_member(params, "value");
    const std::string typed      = value != nullptr && value->is_string()
        ? value->get<std::string>()
        : "";
    {
        std::lock_guard lock(_mutex);
        AcpSession* session = find_session(session_id);
        if (session == nullptr) {
            reply_error(
                id, ERR_RESOURCE_NOT_FOUND, "unknown session", session_id);
            return;
        }
        if (config_id == "mode" && (typed == "plan" || typed == "build")) {
            session->state->session->set_mode(
                typed == "plan" ? Session::Mode::PLAN : Session::Mode::BUILD);
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
        }
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

void AcpInterface::install_observer(AcpSession& session)
{
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
                frame = current->updates.tool_call_update(call);
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
        return session.updates.agent_message(event.text, 0);
    case StreamEvent::Kind::REASONING:
        return session.updates.agent_thought(event.text, 0);
    case StreamEvent::Kind::TOOL_CALL: {
        // Args are complete; TOOL_CALL_START is skipped because its
        // args are still streaming.
        imza::ToolCall snapshot;
        snapshot.call_id = event.tool_call.id;
        snapshot.name    = event.tool_call.name;
        snapshot.args    = event.tool_call.args;
        snapshot.phase   = imza::ToolCall::Phase::EXECUTING;
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
        std::vector<std::string> texts;
        if (content != nullptr && content->is_array()) {
            for (const JsonValue& block : content->get<JsonValue::array_t>()) {
                if (string_member(block, "type") == "text") {
                    texts.push_back(string_member(block, "text"));
                }
            }
        }
        for (std::size_t index = 0; index < form->size(); ++index) {
            QuestionAnswer entry;
            entry.prompt = (*form)[index].prompt;
            if (index < texts.size()) {
                if ((*form)[index].free_text) {
                    entry.free_text = texts[index];
                } else {
                    std::size_t start = 0;
                    for (;;) {
                        const std::size_t comma
                            = texts[index].find(", ", start);
                        entry.selected.push_back(texts[index].substr(start,
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
}

} // namespace imza::acp
