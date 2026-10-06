#include "tools/mcp_manager.h"

#include <utility>

namespace imza {

namespace {

    McpSession make_session(const McpServerConfig& config)
    {
        McpEndpoint endpoint;
        endpoint.url          = config.url;
        endpoint.bearer_token = config.bearer_token;
        for (const auto& [name, value] : config.headers) {
            endpoint.headers.push_back(name + ": " + value);
        }
        return McpSession { std::move(endpoint) };
    }

} // namespace

McpManager::McpManager(std::map<std::string, McpServerConfig> servers)
{
    for (auto& [id, config] : servers) {
        config.id = id;
        if (config.label.empty()) {
            config.label = id;
        }
        // In-place: ServerEntry holds mutexes and is not movable.
        ServerEntry& entry = _servers[id];
        entry.config       = std::move(config);
        entry.state        = entry.config.enabled ? McpServerState::OFFLINE
                                                  : McpServerState::DISABLED;
    }
}

McpManager::~McpManager()
{
    for (auto& [id, entry] : _servers) {
        std::lock_guard io_lock(entry.io);
        mcp_end_session(entry.session);
    }
}

const McpManager::ServerEntry* McpManager::find(const std::string& id) const
{
    const auto found = _servers.find(id);
    return found == _servers.end() ? nullptr : &found->second;
}

McpManager::ServerEntry* McpManager::find(const std::string& id)
{
    return const_cast<ServerEntry*>(std::as_const(*this).find(id));
}

void McpManager::connect(const std::string& id)
{
    ServerEntry* entry = find(id);
    if (entry == nullptr) {
        return;
    }
    {
        std::lock_guard state_lock(entry->state_mutex);
        if (!entry->config.enabled
            || entry->state == McpServerState::CONNECTING) {
            return;
        }
        entry->state  = McpServerState::CONNECTING;
        entry->detail = "";
    }
    _changed.publish();

    McpSession session = make_session(entry->config);
    std::vector<McpToolDefinition> tools;
    std::string detail;
    {
        std::lock_guard io_lock(entry->io);
        Status st = mcp_initialize(session, detail);
        if (st == Status::OK) {
            st = mcp_list_tools(session, tools, detail);
        }
        std::lock_guard state_lock(entry->state_mutex);
        if (st == Status::OK) {
            entry->session = std::move(session);
            entry->tools   = std::move(tools);
            entry->state   = McpServerState::CONNECTED;
        } else {
            entry->session = McpSession { };
            entry->tools.clear();
            entry->state  = McpServerState::FAILED;
            entry->detail = detail;
        }
    }
    _changed.publish();
}

void McpManager::disconnect(const std::string& id)
{
    ServerEntry* entry = find(id);
    if (entry == nullptr) {
        return;
    }
    {
        std::lock_guard io_lock(entry->io);
        mcp_end_session(entry->session);
        std::lock_guard state_lock(entry->state_mutex);
        entry->tools.clear();
        entry->state  = entry->config.enabled ? McpServerState::OFFLINE
                                              : McpServerState::DISABLED;
        entry->detail = "";
    }
    _changed.publish();
}

Status McpManager::call(const std::string& id, const std::string& tool,
    const JsonValue& arguments, McpToolCallResult& out, std::string& detail)
{
    ServerEntry* entry = find(id);
    if (entry == nullptr) {
        detail = "unknown mcp server '" + id + "'";
        return Status::CONFIG_ERROR;
    }
    std::lock_guard io_lock(entry->io);
    {
        std::lock_guard state_lock(entry->state_mutex);
        if (entry->state != McpServerState::CONNECTED) {
            detail = entry->state == McpServerState::DISABLED
                ? "mcp server '" + id + "' is disabled"
                : "mcp server '" + id + "' is not connected";
            return Status::CONFIG_ERROR;
        }
    }
    return mcp_call_tool(entry->session, tool, arguments, out, detail);
}

std::vector<McpServerSnapshot> McpManager::snapshot() const
{
    std::vector<McpServerSnapshot> out;
    out.reserve(_servers.size());
    for (const auto& [id, entry] : _servers) {
        std::lock_guard state_lock(entry.state_mutex);
        out.push_back(McpServerSnapshot { id, entry.config.label,
            entry.config.description, entry.detail, entry.state,
            entry.tools.size() });
    }
    return out;
}

std::optional<std::vector<McpToolDefinition>> McpManager::tools(
    const std::string& id) const
{
    const ServerEntry* entry = find(id);
    if (entry == nullptr) {
        return std::nullopt;
    }
    std::lock_guard state_lock(entry->state_mutex);
    if (entry->state != McpServerState::CONNECTED) {
        return std::nullopt;
    }
    return entry->tools;
}

Signal<>::Subscription McpManager::subscribe(std::function<void()> callback)
{
    return _changed.subscribe(std::move(callback));
}

} // namespace imza
