#pragma once

#include "common/imza_signal.h"
#include "common/types.h"
#include "network/mcp.h"
#include "platform/config.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace imza {

enum class McpServerState { OFFLINE, CONNECTING, CONNECTED, FAILED, INACTIVE };

enum class McpSignInStatus { NONE, RUNNING, FAILED };

struct McpServerSnapshot {
    std::string id;
    std::string label;
    std::string description;
    std::string detail; // failure reason when state == FAILED
    McpServerState state   = McpServerState::OFFLINE;
    std::size_t tool_count = 0;
    bool autoload          = false;
    // The server authenticates through browser sign-in (stored OAuth
    // credentials, or a catalogue entry that requires them).
    bool oauth = false;
    // Browser sign-in progress; RUNNING carries the phase in
    // sign_in_detail, FAILED the reason.
    McpSignInStatus sign_in = McpSignInStatus::NONE;
    std::string sign_in_detail;
};

// Effects injected at the composition boundary: persisting refreshed or
// signed-in credentials (without a manager reload), and opening the
// user's browser. Defaults make both no-ops so tests stay hermetic.
struct McpManagerHooks {
    std::function<void(const McpServerConfig& server)> persist_server;
    std::function<bool(const std::string& url)> open_browser;
};

// Owns one MCP session per configured server. Long-lived
// ApplicationComponent: connect/disconnect run on manager-owned workers
// (the UI thread never blocks) and announce through the signal; call()
// blocks on network I/O and is for agent workers. Entry I/O serializes on
// a per-entry mutex; reload() applies config changes without network I/O
// (abandoned sessions are cleaned up server-side).
class McpManager final : public ApplicationComponent {
public:
    explicit McpManager(std::map<std::string, McpServerConfig> servers,
        McpManagerHooks hooks = { });
    ~McpManager();

    McpManager(const McpManager&)            = delete;
    McpManager& operator=(const McpManager&) = delete;

    // Applies config changes: added servers appear offline, removed
    // servers are dropped (their sessions abandoned), kept servers keep
    // their session unless the connection fields changed. Never performs
    // network I/O. Announces when anything moved.
    void reload(std::map<std::string, McpServerConfig> servers);

    // Starts a handshake + tools/list on a worker; unknown, inactive,
    // connecting and already-connected ids are no-ops.
    void connect(const std::string& id);

    // Connects every enabled server flagged for autoload.
    void autoload();

    // Marks the server offline immediately and ends the remote session on
    // a worker (best-effort DELETE; servers may refuse it).
    void disconnect(const std::string& id);

    // Starts the browser OAuth flow (network/mcp_oauth) on a worker;
    // stdio, unknown, and already-running ids are no-ops. On success the
    // entry is enabled, credentials are persisted through the hook, and
    // the handshake starts.
    void sign_in(const std::string& id);

    // Aborts a running sign-in (the worker observes the flag).
    void cancel_sign_in(const std::string& id);

    // Requires a connected server; tool execution errors (isError) come
    // back through `out` with Status::OK.
    Status call(const std::string& id, const std::string& tool,
        const JsonValue& arguments, McpToolCallResult& out,
        std::string& detail);

    std::vector<McpServerSnapshot> snapshot() const;
    // Tool inventory of a connected server; nullopt when unknown or not
    // connected.
    std::optional<std::vector<McpToolDefinition>> tools(
        const std::string& id) const;

    // Callbacks run on manager worker threads; keep the Subscription and
    // marshal UI work through the main-thread queue.
    [[nodiscard]] Signal<>::Subscription subscribe(
        std::function<void()> callback);

private:
    // shared_ptr: a call or handshake keeps its entry alive across reload.
    struct ServerEntry {
        McpServerConfig config;
        std::mutex io; // serializes session I/O (handshake, calls, reset)
        mutable std::mutex state_mutex; // guards the snapshot fields below
        std::atomic_bool stream_cancel {
            false
        }; // aborts the in-flight notification stream
        std::atomic_bool sign_in_cancel {
            false
        }; // aborts the in-flight browser sign-in
        McpServerState state   = McpServerState::OFFLINE;
        int reconnect_attempts = 0;
        std::string detail;
        McpSignInStatus sign_in = McpSignInStatus::NONE;
        std::string sign_in_detail;
        McpSession session;
        std::vector<McpToolDefinition> tools;
    };

    std::shared_ptr<ServerEntry> find(const std::string& id) const;
    bool still_tracked(
        const std::string& id, const std::shared_ptr<ServerEntry>& entry) const;
    void spawn(std::function<void()> work);
    void run_handshake(
        std::shared_ptr<ServerEntry> entry, bool force_token_refresh = false);
    void run_listener(std::shared_ptr<ServerEntry> entry);
    void run_sign_in(std::shared_ptr<ServerEntry> entry);
    // Refreshes the stored OAuth access token when it is missing its
    // window (or `forced`), updating and persisting the entry config.
    bool refresh_entry_tokens(const std::shared_ptr<ServerEntry>& entry,
        bool forced, std::string& detail);
    bool wait_interruptible(long ms) const;

    std::atomic_bool _stopping { false };
    mutable std::mutex _map_mutex;
    std::map<std::string, std::shared_ptr<ServerEntry>> _servers;
    // Guards _workers: workers spawn follow-up work (retries, listeners)
    // from their own threads, so appends race the destructor's join.
    std::mutex _workers_mutex;
    std::vector<std::thread> _workers;
    McpManagerHooks _hooks;
    Signal<> _changed;
};

} // namespace imza
