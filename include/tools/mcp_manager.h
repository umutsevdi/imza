#pragma once

#include "common/imza_signal.h"
#include "common/types.h"
#include "network/mcp.h"
#include "platform/config.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace imza {

enum class McpServerState { OFFLINE, CONNECTING, CONNECTED, FAILED, DISABLED };

struct McpServerSnapshot {
    std::string id;
    std::string label;
    std::string description;
    std::string detail; // failure reason when state == FAILED
    McpServerState state   = McpServerState::OFFLINE;
    std::size_t tool_count = 0;
};

// Owns one MCP session per configured server. Long-lived
// ApplicationComponent: the map is fixed at construction (config reloads
// mean a new manager), state changes are announced on the signal, and UI
// reads snapshots only. connect/call block on network I/O — invoke them
// from a worker thread, never the UI thread. Calls and handshakes against
// one server serialize on that entry's I/O mutex; the manager itself holds
// no threads.
class McpManager final : public ApplicationComponent {
public:
    explicit McpManager(std::map<std::string, McpServerConfig> servers);
    ~McpManager();

    McpManager(const McpManager&)            = delete;
    McpManager& operator=(const McpManager&) = delete;

    // Blocking handshake + tools/list. Unknown ids are ignored; a disabled
    // server stays off until re-enabled through config.
    void connect(const std::string& id);
    void disconnect(const std::string& id);

    // Requires a connected server; tool execution errors (isError) come
    // back through `out` with Status::OK.
    Status call(const std::string& id, const std::string& tool,
        const JsonValue& arguments, McpToolCallResult& out,
        std::string& detail);

    std::vector<McpServerSnapshot> snapshot() const;
    // Tool inventory of a connected server; nullopt when unknown or offline.
    std::optional<std::vector<McpToolDefinition>> tools(
        const std::string& id) const;

    [[nodiscard]] Signal<>::Subscription subscribe(
        std::function<void()> callback);

private:
    struct ServerEntry {
        McpServerConfig config;
        std::mutex io; // serializes session I/O (handshake, calls, teardown)
        mutable std::mutex state_mutex; // guards the snapshot fields below
        McpServerState state = McpServerState::OFFLINE;
        std::string detail;
        McpSession session;
        std::vector<McpToolDefinition> tools;
    };

    const ServerEntry* find(const std::string& id) const;
    ServerEntry* find(const std::string& id);

    std::map<std::string, ServerEntry> _servers;
    Signal<> _changed;
};

} // namespace imza
