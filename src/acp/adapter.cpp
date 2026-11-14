#include "acp/adapter.h"

#include "app/flows.h"
#include "app/slash_commands.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "platform/config.h"
#include "platform/file_lock.h"
#include "providers/provider_store.h"
#include "workspace/skills.h"

#include <algorithm>
#include <ctime>
#include <future>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <variant>

namespace imza::acp {

using imza::parse_rpc_frame;
using imza::rpc_error;
using imza::rpc_notification;
using imza::rpc_request;
using imza::rpc_response;

namespace {

    constexpr const char* PERMISSION_METHOD = "session/request_permission";
    constexpr const char* ELICIT_METHOD     = "elicitation/create";

    std::string mode_name(Session::Mode mode)
    {
        return mode == Session::Mode::PLAN ? "plan" : "build";
    }

    std::string document_label(SessionDoc::Kind kind)
    {
        switch (kind) {
        case SessionDoc::Kind::PLAN: return "Plan document:";
        case SessionDoc::Kind::BUILD: return "Build log:";
        case SessionDoc::Kind::HANDOFF: return "Handoff report:";
        }
        return "Document:";
    }

    std::string string_member(const JsonValue& value, std::string_view key)
    {
        const JsonValue* found = find_member(value, key);
        if (found == nullptr || !found->is_string()) {
            return { };
        }
        return found->get<std::string>();
    }

    // Converts a stored local "%Y-%m-%d %H:%M:%S" stamp to the RFC 3339
    // form ACP's updatedAt requires, in UTC so the Z suffix is honest.
    // Unparseable input yields an empty string.
    std::string rfc3339_from_local(std::string_view local_stamp)
    {
        std::tm tm { };
        std::istringstream in { std::string(local_stamp) };
        in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (in.fail()) {
            return { };
        }
        tm.tm_isdst             = -1;
        const std::time_t value = std::mktime(&tm);
        if (value == static_cast<std::time_t>(-1)) {
            return { };
        }
        const std::tm utc = utc_tm(value);
        std::ostringstream out;
        out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return std::move(out).str();
    }

    // The form-mode schema for a question form: one string property per
    // card, enum-constrained when the card offers choices.
    JsonValue elicitation_schema(const QuestionForm& form)
    {
        JsonValue::object_t properties;
        for (std::size_t index = 0; index < form.size(); ++index) {
            const QuestionCard& card = form[index];
            JsonValue field;
            field["type"]        = "string";
            field["description"] = card.prompt;
            if (!card.options.empty()) {
                field["enum"] = JsonValue(JsonValue::array_t(
                    std::begin(card.options), std::end(card.options)));
            }
            properties["card" + std::to_string(index + 1)] = std::move(field);
        }
        JsonValue schema     = json_object();
        schema["type"]       = "object";
        schema["properties"] = std::move(properties);
        return schema;
    }

    // One card's answer text, split on ", " when the card is a choice list.
    QuestionAnswer answer_for(const QuestionCard& card, const std::string& text)
    {
        QuestionAnswer entry;
        entry.prompt = card.prompt;
        if (text.empty()) {
            return entry;
        }
        if (card.free_text) {
            entry.free_text = text;
            return entry;
        }
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = text.find(", ", start);
            entry.selected.push_back(text.substr(start,
                comma == std::string::npos ? std::string::npos
                                           : comma - start));
            if (comma == std::string::npos) {
                return entry;
            }
            start = comma + 2;
        }
    }

    // Accepted form content is one object keyed by the requested schema's
    // property names ("card1", "card2", ...); a legacy client's
    // array-of-text-blocks is tolerated too.
    ModalAnswer answers_from_elicitation(
        const QuestionForm& form, const JsonValue& raw)
    {
        std::map<std::string, std::string> fields;
        const JsonValue* content = find_member(raw, "content");
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
        ModalAnswer answer;
        for (std::size_t index = 0; index < form.size(); ++index) {
            const std::string key = "card" + std::to_string(index + 1);
            const auto found      = fields.find(key);
            answer.cards.push_back(answer_for(form[index],
                found == fields.end() ? std::string { } : found->second));
        }
        return answer;
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
    std::string line = rpc_response(id, result);
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
        JsonValue error   = json_object();
        error["code"]     = static_cast<double>(code);
        error["message"]  = std::string(message);
        JsonValue data    = json_object();
        data["sessionId"] = session_id;
        error["data"]     = data;
        JsonValue frame;
        frame["jsonrpc"] = "2.0";
        frame["id"]      = id;
        frame["error"]   = error;
        line             = json_dump(frame);
    } else {
        line = rpc_error(id, code, message);
    }
    std::lock_guard lock(_mutex);
    if (_write) {
        _write(line);
    }
}

std::shared_ptr<AcpInterface::AcpSession> AcpInterface::find_session(
    const std::string& id)
{
    const auto found = _sessions.find(id);
    return found == _sessions.end() ? nullptr : found->second;
}

void AcpInterface::push(const std::string& session_id,
    const std::function<std::string(AcpSession&)>& build)
{
    std::string frame;
    {
        std::lock_guard lock(_mutex);
        auto current = find_session(session_id);
        if (current == nullptr) {
            return;
        }
        frame = build(*current);
    }
    if (frame.empty()) {
        return;
    }
    std::lock_guard lock(_mutex);
    if (_write && find_session(session_id) != nullptr) {
        _write(frame);
    }
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
    } else {
        // Unknown methods and unimplemented extension methods both answer
        // -32601, per the error page.
        reply_error(id, RPC_METHOD_NOT_FOUND, "method not found");
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
    auto session = find_session(session_id);
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
        auto session = find_session(session_id);
        if (session == nullptr) {
            cancelled = true;
            return json_object();
        }
        call_id                      = _next_client_call++;
        session->pending_client_call = call_id;
        session->client_reply        = std::promise<JsonValue> { };
        session->awaiting_reply      = true;
        reply                        = session->client_reply.get_future();
    }
    const std::string line = rpc_request(call_id, method, params);
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
        return json_object();
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
        if (session->pending_client_call == call_id
            && session->awaiting_reply) {
            JsonValue result
                = message.result.has_value() ? *message.result : JsonValue { };
            session->client_reply.set_value(std::move(result));
            session->awaiting_reply      = false;
            session->pending_client_call = std::nullopt;
            return;
        }
    }
}

void AcpInterface::handle_initialize(
    const JsonValue& id, const JsonValue& params)
{
    const InitializeRequest request = InitializeRequest::parse(params);
    _elicitation_form               = request.client.elicitation_form;
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
    reply_error(id, RPC_INVALID_REQUEST,
        "request received before initialization: " + std::string(method));
    return false;
}

JsonValue AcpInterface::available_commands_payload(AcpSession& session)
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
    if (const auto skills = session.state->environment->sorted_skills()) {
        for (const Skill& skill : *skills) {
            commands.emplace_back(skill.name, skill.description,
                "Mention as $" + skill.name + " in a prompt");
        }
    }
    return session.updates.available_commands(commands);
}

void AcpInterface::send_available_commands(AcpSession& session)
{
    std::vector<std::string> skill_names;
    if (const auto skills = session.state->environment->sorted_skills()) {
        for (const Skill& skill : *skills) {
            skill_names.push_back(skill.name);
        }
    }
    bool skip;
    {
        std::lock_guard lock(session.advertised_mutex);
        skip = session.skills_advertised
            && skill_names == session.advertised_skills;
        session.advertised_skills = skill_names;
        session.skills_advertised = true;
    }
    if (skip) {
        return;
    }
    std::string line = session.updates.available_commands_payload(
        available_commands_payload(session));
    if (_write) {
        _write(line);
    }
}

void AcpInterface::send_available_commands_for(const std::string& session_id)
{
    std::lock_guard lock(_mutex);
    auto session = find_session(session_id);
    if (session == nullptr) {
        return;
    }
    send_available_commands(*session);
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
    const JsonValue payload  = dump_config_options(config_options_for(session));
    const std::string dumped = json_dump(payload);
    if (dumped == session.sent_config_options) {
        return;
    }
    session.sent_config_options = dumped;
    std::string line            = session.updates.config_option_update(payload);
    if (_write) {
        _write(line);
    }
}

void AcpInterface::send_config_options_for(const std::string& session_id)
{
    std::lock_guard lock(_mutex);
    auto session = find_session(session_id);
    if (session == nullptr) {
        return;
    }
    send_config_options(*session);
}

void AcpInterface::fill_session_state(
    const std::string& session_id, JsonValue& result)
{
    std::lock_guard lock(_mutex);
    auto session = find_session(session_id);
    if (session == nullptr) {
        return;
    }
    const JsonValue payload = dump_config_options(config_options_for(*session));
    session->sent_config_options = json_dump(payload);
    result["configOptions"]      = payload;
    result["modes"]
        = dump_session_modes(mode_name(session->state->session->mode()));
    // Clients build the palette from the setup response, so the reply
    // must carry the command list, not just later updates.
    result["availableCommands"] = available_commands_payload(*session);
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
                auto session = find_session(*routing);
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

    std::shared_ptr<AcpSession> observer_target;
    {
        std::lock_guard lock(_mutex);
        auto [entry, inserted] = _sessions.try_emplace(protocol_id,
            std::make_shared<AcpSession>(
                protocol_id, state, UpdateBuilder(protocol_id)));
        if (!inserted) {
            reply_error(id, RPC_INTERNAL_ERROR, "session id collision");
            return;
        }
        observer_target = entry->second;
    }
    // Outside the adapter mutex: the signals subscribed here replay to
    // ready subscribers synchronously, and their callbacks take _mutex.
    install_observer(*observer_target);
    // Carried in the reply so the client renders its switchers at once.
    JsonValue result    = json_object();
    result["sessionId"] = protocol_id;
    fill_session_state(protocol_id, result);
    reply(id, result);
    {
        std::lock_guard lock(_mutex);
        auto session = find_session(protocol_id);
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
            auto session = find_session(protocol_id);
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

    std::shared_ptr<AcpSession> observer_target;
    {
        std::lock_guard lock(_mutex);
        auto [entry, inserted] = _sessions.try_emplace(protocol_id,
            std::make_shared<AcpSession>(
                protocol_id, state, UpdateBuilder(protocol_id)));
        if (!inserted) {
            reply_error(id, RPC_INTERNAL_ERROR, "session id collision");
            return;
        }
        entry->second->lock = std::move(std::get<FileLock>(guard));
        observer_target     = entry->second;
    }
    // Outside the adapter mutex: see handle_session_new.
    install_observer(*observer_target);
    if (method != "session/resume") {
        // Replay precedes the response, per spec.
        std::lock_guard lock(_mutex);
        auto session = find_session(protocol_id);
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
                frames.push_back(rpc_notification("session/update",
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
    JsonValue result = json_object();
    fill_session_state(protocol_id, result);
    reply(id, result);
}

void AcpInterface::handle_session_prompt(
    const JsonValue& id, const JsonValue& params)
{
    const PromptRequest request = PromptRequest::parse(params);
    std::shared_ptr<AcpSession> session;
    {
        std::lock_guard lock(_mutex);
        session = find_session(request.session_id);
    }
    if (session == nullptr) {
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
    // second session's frames queue behind this prompt forever. The
    // thread holds its own reference, so a concurrent session/close
    // cannot pull the session out from under it.
    std::lock_guard lock(_mutex);
    _prompt_threads.emplace_back([this, id, session, text = std::move(text),
                                     attachments = std::move(attachments)]() {
        run_prompt_turn(id, session, std::move(text), std::move(attachments));
    });
}

void AcpInterface::run_prompt_turn(const JsonValue& id,
    std::shared_ptr<AcpSession> session, std::string text,
    std::vector<Attachment> attachments)
{
    std::shared_ptr<ApplicationState> state = session->state;
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
        if (!session->lock.has_value()) {
            auto guard = acquire_file_lock(lock_path_for(path),
                FileLockRequest { FileLockMode::EXCLUSIVE, false });
            if (std::holds_alternative<FileLock>(guard)) {
                std::lock_guard lock(_mutex);
                session->lock = std::move(std::get<FileLock>(guard));
            }
        }
    }
    JsonValue result     = json_object();
    result["stopReason"] = std::string(stop_reason_name(reason));
    reply(id, result);
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
        reply_error(id, RPC_INVALID_PARAMS,
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
        // Checked before canonicalizing: weakly_canonical resolves a
        // relative path against the cwd and would return it absolute.
        const std::filesystem::path requested = path_from_utf8(requested_cwd);
        if (!requested.is_absolute()) {
            reply_error(id, RPC_INVALID_PARAMS, "cwd must be an absolute path");
            return;
        }
        std::error_code error;
        const std::filesystem::path filter
            = std::filesystem::weakly_canonical(requested, error);
        if (error || filter.empty()) {
            reply_error(id, RPC_INVALID_PARAMS, "cwd must be an absolute path");
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
            reply_error(id, RPC_INVALID_PARAMS, "invalid cursor");
            return;
        }
    }
    JsonValue result = json_object();
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
        reply(id, json_object());
        return;
    }
    {
        // Drop this process's own guard first, or the delete would fail
        // on a lock we hold ourselves.
        std::lock_guard lock(_mutex);
        if (auto session = find_session(session_id); session != nullptr) {
            session->lock.reset();
        }
    }
    switch (delete_saved_session(found->path)) {
    case DeleteSessionResult::OK: reply(id, json_object()); return;
    case DeleteSessionResult::INVALID_PATH:
        reply_error(id, RPC_INVALID_PARAMS, "invalid session path", session_id);
        return;
    case DeleteSessionResult::REMOVE_FAILED:
        reply_error(id, RPC_INTERNAL_ERROR,
            "session is open in another imza process", session_id);
        return;
    }
    reply(id, json_object());
}

void AcpInterface::handle_session_close(
    const JsonValue& id, const JsonValue& params)
{
    const std::string session_id = string_member(params, "sessionId");
    std::shared_ptr<AcpSession> session;
    {
        std::lock_guard lock(_mutex);
        // Idempotent: an unknown or already-closed session succeeds.
        const auto found = _sessions.find(session_id);
        if (found == _sessions.end()) {
            reply(id, json_object());
            return;
        }
        session = found->second;
        // Dropping the map's reference releases the file lock; a running
        // prompt thread keeps the session itself alive to finish cleanly.
        _sessions.erase(found);
    }
    // Cancel any work as if session/cancel had been sent.
    session->state->session->request_interrupt();
    reply(id, json_object());
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
        reply_error(id, RPC_INVALID_PARAMS, "unknown configId: " + config_id,
            session_id);
        return;
    }
    if (config_id == "thought_level" && !typed.empty()
        && !std::ranges::contains(REASONING_EFFORT_CHOICES, typed)) {
        reply_error(
            id, RPC_INVALID_PARAMS, "unknown value: " + typed, session_id);
        return;
    }
    std::shared_ptr<ApplicationState> mode_target;
    std::shared_ptr<ApplicationState> options_target;
    {
        std::lock_guard lock(_mutex);
        auto session = find_session(session_id);
        if (session == nullptr) {
            reply_error(
                id, ERR_RESOURCE_NOT_FOUND, "unknown session", session_id);
            return;
        }
        if (switching_mode) {
            mode_target = session->state;
        } else if (config_id == "skip_permissions") {
            // Does not reach the provider store, so no callback re-entry.
            session->state->permissions->set_skip_permissions(typed_bool);
        } else {
            options_target = session->state;
        }
    }
    // set_mode publishes synchronously on this thread and the observer
    // re-locks _mutex, so it must run outside the lock. Same rule for
    // provider mutations: the provider subscription callback takes the
    // adapter mutex when the change notification lands.
    if (mode_target != nullptr) {
        mode_target->session->set_mode(
            typed == "plan" ? Session::Mode::PLAN : Session::Mode::BUILD);
    }
    if (options_target != nullptr) {
        if (config_id == "thought_level" && !typed.empty()) {
            options_target->providers->set_reasoning_effort(
                to_wire_effort(typed));
        } else if (config_id == "model" && !typed.empty()) {
            if (const auto selection
                = options_target->providers->active_selection();
                selection != std::nullopt) {
                options_target->providers->select_model(
                    { selection->connection_id, typed });
            }
        }
    }
    JsonValue result = json_object();
    // The reply carries the complete new state; the notification pushes
    // it to the client's switchers.
    {
        std::lock_guard lock(_mutex);
        auto session = find_session(session_id);
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
        auto current = find_session(session_id);
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
        auto current = find_session(session_id);
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
    // Every callback builds a frame from the live session and hands it to
    // push(); mode and title fire on whichever thread made the change, the
    // rest on the session's own workers.
    const std::string id = session.id;
    session.mode_subscription
        = session.state->session->subscribe_to_mode_change([this, id] {
              push(id, [](AcpSession& current) {
                  return current.updates.current_mode(
                      mode_name(current.state->session->mode()));
              });
          });
    session.title_subscription
        = session.state->session->subscribe_to_title_change([this, id] {
              push(id, [](AcpSession& current) {
                  const std::string title = current.state->session->title();
                  return title.empty() ? std::string { }
                                       : current.updates.session_info(title);
              });
          });
    // The todo list lives outside the transcript, so the client would
    // never see plan changes without this push.
    session.todo_subscription
        = session.state->session->subscribe_to_todo_change([this, id] {
              push(id, [](AcpSession& current) {
                  return current.updates.plan(*current.state->session->todo());
              });
          });
    // Documents have no client-side viewer; each new or changed one
    // reaches the chat as a markdown chunk.
    session.documents_subscription
        = session.state->session->subscribe_to_documents_change([this, id] {
              push(id, [this](AcpSession& current) {
                  const auto documents = current.state->session->documents();
                  std::vector<std::pair<int, std::string>> now;
                  now.reserve(documents->size());
                  for (const SessionDoc& doc : *documents) {
                      now.emplace_back(static_cast<int>(doc.kind), doc.content);
                  }
                  if (now == current.pushed_documents) {
                      return std::string { };
                  }
                  // Push only the suffix that differs; a rewritten
                  // document falls back to a full resend.
                  std::size_t start = 0;
                  const std::size_t common
                      = std::min(now.size(), current.pushed_documents.size());
                  while (start < common
                      && now[start] == current.pushed_documents[start]) {
                      ++start;
                  }
                  current.pushed_documents = now;
                  if (start >= now.size()) {
                      return std::string { };
                  }
                  std::string frames;
                  for (std::size_t i = start; i < now.size(); ++i) {
                      ++current.message_id;
                      frames += current.updates.document_chunk(
                          document_label(
                              static_cast<SessionDoc::Kind>(now[i].first)),
                          now[i].second, current.message_id);
                      frames += '\n';
                  }
                  return frames;
              });
          });
    session.skills_subscription
        = session.state->environment->subscribe_to_system_change(
            [this, id] { send_available_commands_for(id); });
    session.provider_subscription
        = session.state->providers->subscribe_to_change(
            [this, id] { send_config_options_for(id); });
    session.compaction_subscription
        = session.state->session->subscribe_to_compaction_change([this, id] {
              if (!_compaction_capable) {
                  return;
              }
              push(id, [](AcpSession& current) {
                  // Re-snapshot: the event that fired may not be newest.
                  const auto items = current.state->session->items();
                  for (const ConversationItem& item :
                      *items | std::views::reverse) {
                      const auto* entry = std::get_if<CompactionEvent>(&item);
                      if (entry == nullptr) {
                          continue;
                      }
                      const auto compaction_id
                          = static_cast<std::int64_t>(entry->id);
                      switch (entry->status) {
                      case CompactionEvent::Status::RUNNING:
                          return current.updates.compaction_in_progress(
                              compaction_id);
                      case CompactionEvent::Status::COMPLETED:
                          return current.updates.compaction_completed(
                              compaction_id,
                              current.state->session->compacted_summary());
                      case CompactionEvent::Status::FAILED:
                          return current.updates.compaction_failed(
                              compaction_id, "");
                      }
                  }
                  return std::string { };
              });
          });
    session.usage_subscription
        = session.state->session->subscribe_to_usage_change(
            [this, id] { send_usage_update_for(id); });
    session.notice_subscription = session.state->session->subscribe_to_notice(
        [this, id](const std::string& message) {
            if (_notices_capable) {
                send_notice_for(id, "info", message);
            }
        });
    session.error_subscription = session.state->session->subscribe_to_error(
        [this, id](const std::string& message) {
            if (_notices_capable) {
                send_notice_for(id, "error", message);
            }
        });
    // Tool results land outside the stream.
    session.state->session->set_tool_result_observer(
        [this, id](const ToolCall& call) {
            push(id, [&call](AcpSession& current) {
                return current.updates.tool_call_update(call);
            });
        });
    session.state->session->set_stream_observer(
        [this, id](const StreamEvent& event) {
            push(id, [this, &event](AcpSession& current) {
                return stream_frame(current, event).value_or(std::string { });
            });
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
        JsonValue params    = json_object();
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
        JsonValue params          = json_object();
        params["sessionId"]       = session.id;
        params["mode"]            = "form";
        params["message"]         = form->front().prompt;
        params["requestedSchema"] = elicitation_schema(*form);
        bool cancelled            = false;
        const JsonValue raw     = call_client(ELICIT_METHOD, params, cancelled);
        const JsonValue* action = find_member(raw, "action");
        const bool accepted     = !cancelled && action != nullptr
            && action->is_string() && action->get<std::string>() == "accept";
        done.set_value(accepted
                ? ModalResult { answers_from_elicitation(*form, raw) }
                : ModalResult { std::monostate { } });
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
            session->state->session->request_interrupt();
            if (session->awaiting_reply) {
                session->client_reply.set_value(JsonValue { });
                session->awaiting_reply = false;
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
