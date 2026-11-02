#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "acp/protocol.h"
#include "acp/updates.h"
#include "acp/wire.h"
#include "app/application_state.h"
#include "common/imza_signal.h"
#include "platform/file_lock.h"

namespace imza::acp {

// The ACP agent: one child ApplicationState per protocol session, driven
// through the same session/turn/modal machinery as the TUI.
class AcpInterface final : public Subsystem {
public:
    AcpInterface(ApplicationState& root, Config config);
    ~AcpInterface() override { stop(); }
    void stop() override;

    using WriteFn = std::function<void(std::string_view)>;
    void set_writer(WriteFn write);

    void handle_request(
        const JsonValue& id, std::string_view method, const JsonValue& params);
    void handle_notification(std::string_view method, const JsonValue& params);
    void handle_response(const Message& message);

    // Server -> client call; blocks until the client answers or cancels.
    JsonValue call_client(
        std::string_view method, const JsonValue& params, bool& cancelled);

private:
    struct AcpSession {
        std::string id;
        std::shared_ptr<ApplicationState> state;
        UpdateBuilder updates;
        // Opaque per-message id for streamed chunks; bumped when a new
        // message begins (a tool call boundary).
        std::int64_t message_id = 1;
        // The client call this session's turn worker is blocked on.
        std::optional<std::int64_t> pending_client_call;
        std::promise<JsonValue> client_reply;
        bool awaiting_reply = false;
        // Chat-active guard on the session file: refuses a second writer
        // (TUI or another ACP connection) and shows the session as locked.
        // Released when the last reference to the session dies.
        std::optional<FileLock> lock;
        Signal<>::Subscription mode_subscription;
        Signal<>::Subscription title_subscription;
        Signal<>::Subscription todo_subscription;
        Signal<>::Subscription compaction_subscription;
        Signal<>::Subscription usage_subscription;
        Signal<const std::string&>::Subscription notice_subscription;
        Signal<const std::string&>::Subscription error_subscription;
    };

    std::shared_ptr<AcpSession> find_session(const std::string& id);
    // Runs `build` against the live session under the adapter mutex and
    // writes whatever frame it returns (an empty string means "nothing to
    // say"). No-op once the session is gone. Every observer callback is
    // this shape.
    void push(const std::string& session_id,
        const std::function<std::string(AcpSession&)>& build);
    void reply(const JsonValue& id, const JsonValue& result);
    void reply_error(const JsonValue& id, int code, std::string_view message,
        const std::string& session_id = { });

    void handle_initialize(const JsonValue& id, const JsonValue& params);
    // False (with -32600 sent) when a request other than initialize
    // arrives before initialization completes.
    bool require_initialized(const JsonValue& id, std::string_view method);
    void handle_session_new(const JsonValue& id, const JsonValue& params);
    void handle_session_load(
        const JsonValue& id, std::string_view method, const JsonValue& params);
    void handle_session_prompt(const JsonValue& id, const JsonValue& params);
    // The blocking half of session/prompt, on its own thread so the wire
    // loop keeps reading (cancel, other sessions).
    void run_prompt_turn(const JsonValue& id,
        std::shared_ptr<AcpSession> session, std::string text,
        std::vector<Attachment> attachments);
    void handle_session_list(const JsonValue& id, const JsonValue& params);
    // False (with -32602 already sent) when the request's cwd names a
    // directory other than the process working directory.
    bool accept_cwd(const JsonValue& id, const JsonValue& params);
    void handle_session_delete(const JsonValue& id, const JsonValue& params);
    void handle_session_close(const JsonValue& id, const JsonValue& params);
    // Handles both session/set_config_option and the legacy session/set_mode;
    // `method` selects which field names the target value.
    void handle_set_config_option(
        const JsonValue& id, const JsonValue& params, std::string_view method);

    // Installed as the child state's modal routing: permission prompts
    // and asks reach the client instead of a TUI.
    std::future<ModalResult> request_modal(
        AcpSession& session, ModalPayload payload);

    void install_observer(AcpSession& session);
    std::optional<std::string> stream_frame(
        AcpSession& session, const StreamEvent& event);
    // Context usage pushed to the client on each usage change.
    void send_usage_update_for(const std::string& session_id);
    void send_notice_for(const std::string& session_id, const char* severity,
        const std::string& message);
    // The senders require _mutex held by the caller; config_options_for
    // only reads the session it is given.
    void send_available_commands(AcpSession& session);
    void send_config_options(AcpSession& session);
    std::vector<ConfigOption> config_options_for(AcpSession& session);
    // The session state every lifecycle reply carries: configOptions and
    // the legacy modes block. No-op when the session vanished.
    void fill_session_state(const std::string& session_id, JsonValue& result);

    ApplicationState& _root;
    Config _config;
    WriteFn _write;
    std::mutex _mutex;
    // shared_ptr: a prompt thread keeps its session alive across a
    // concurrent session/close, so erasure needs no deferral protocol.
    std::map<std::string, std::shared_ptr<AcpSession>> _sessions;
    std::vector<std::jthread> _prompt_threads;
    std::int64_t _next_client_call = 1;
    // Advertised by the client at initialize; asks fail closed without it.
    bool _elicitation_form = false;
    // Capability-gated outbound updates; false means never send.
    bool _compaction_capable = false;
    bool _notices_capable    = false;
    bool _boolean_options    = false;
    // False until initialize answers; earlier requests get -32600.
    bool _initialized = false;
    // clientInfo.name, stamped on every session this connection creates and
    // used to scope listings, loads, and deletes.
    std::string _client_label = std::string(ACP_UNNAMED_CLIENT);
};

} // namespace imza::acp
