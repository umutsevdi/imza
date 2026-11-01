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
        // The client call this session's turn worker is blocked on.
        std::optional<std::int64_t> pending_client_call;
        std::promise<JsonValue> client_reply;
        bool awaiting_reply = false;
        // session/close defers erasure while a prompt thread runs.
        std::atomic<bool> prompt_running { false };
        bool close_requested = false;
    };

    AcpSession* find_session(const std::string& id);
    void reply(const JsonValue& id, const JsonValue& result);
    void reply_error(const JsonValue& id, int code, std::string_view message,
        const std::string& session_id = { });

    void handle_initialize(const JsonValue& id, const JsonValue& params);
    void handle_session_new(const JsonValue& id, const JsonValue& params);
    void handle_session_load(
        const JsonValue& id, std::string_view method, const JsonValue& params);
    void handle_session_prompt(const JsonValue& id, const JsonValue& params);
    // The blocking half of session/prompt, on its own thread so the wire
    // loop keeps reading (cancel, other sessions).
    void run_prompt_turn(const JsonValue& id, const std::string& session_id,
        std::shared_ptr<ApplicationState> state, std::string text,
        std::vector<Attachment> attachments);
    void handle_session_list(const JsonValue& id, const JsonValue& params);
    void handle_session_delete(const JsonValue& id, const JsonValue& params);
    void handle_session_close(const JsonValue& id, const JsonValue& params);
    void handle_set_config_option(const JsonValue& id, const JsonValue& params);

    // Installed as the child state's modal routing: permission prompts
    // and asks reach the client instead of a TUI.
    std::future<ModalResult> request_modal(
        AcpSession& session, ModalPayload payload);

    void install_observer(AcpSession& session);
    std::optional<std::string> stream_frame(
        AcpSession& session, const StreamEvent& event);
    // Both require _mutex held by the caller.
    void send_available_commands(AcpSession& session);
    void send_config_options(AcpSession& session);
    std::vector<ConfigOption> config_options_for(AcpSession& session);

    ApplicationState& _root;
    Config _config;
    WriteFn _write;
    std::mutex _mutex;
    std::map<std::string, AcpSession> _sessions;
    std::vector<std::jthread> _prompt_threads;
    std::int64_t _next_client_call = 1;
    // Advertised by the client at initialize; asks fail closed without it.
    bool _elicitation_form = false;
};

} // namespace imza::acp
