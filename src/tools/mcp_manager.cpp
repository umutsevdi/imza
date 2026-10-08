#include "tools/mcp_manager.h"

#include "network/json_io.h"
#include "platform/process.h"
#include "tools/mcp_catalog.h"

#include <utility>

namespace imza {

namespace {

    constexpr long DEFAULT_TIMEOUT_SECS = 25;
    // Reconnect backoff after a failed handshake; 5 attempts then the
    // server stays FAILED until a manual connect resets the count.
    constexpr int MAX_RECONNECT_ATTEMPTS = 5;
    constexpr long RETRY_DELAYS_SECS[]   = { 2, 5, 10, 30, 60 };

    // Effective URL: the explicit one (custom servers) or the bundled
    // catalogue's (reference servers). Empty when a reference no longer
    // resolves; callers surface that as a connection failure.
    std::string resolved_url(const McpServerConfig& config)
    {
        if (!config.url.empty() || config.catalog_id.empty()) {
            return expand_env_vars(config.url);
        }
        const auto entry = find_mcp_catalog_entry(config.catalog_id);
        return entry ? expand_env_vars(entry->url) : std::string { };
    }

    std::string dangling_catalog_detail(const McpServerConfig& config)
    {
        return "catalogue entry '" + config.catalog_id + "' no longer exists";
    }

    // An HTTP endpoint, or a spawned stdio child wrapped in line hooks.
    // nullopt with `detail` set when a stdio process cannot be spawned.
    std::optional<McpSession> make_session(
        const McpServerConfig& config, std::string& detail)
    {
        const long timeout = config.timeout_secs > 0 ? config.timeout_secs
                                                     : DEFAULT_TIMEOUT_SECS;
        if (config.is_stdio()) {
            ProcessOptions options;
            options.argv.push_back(config.command);
            for (const std::string& arg : config.args) {
                options.argv.push_back(expand_env_vars(arg));
            }
            for (const auto& [name, value] : config.env) {
                options.env[name] = expand_env_vars(value);
            }
            options.working_directory = config.working_directory;
            std::string spawn_error;
            std::shared_ptr<Process> process
                = Process::spawn(options, spawn_error);
            if (process == nullptr) {
                detail = "spawn failed: " + spawn_error;
                return std::nullopt;
            }
            McpSession session;
            session.endpoint.timeout_secs = timeout;
            session.stdio.write           = [process](std::string_view line) {
                return process->write_line(line);
            };
            session.stdio.read = [process](std::string& line, long secs) {
                return process->read_line(line, std::chrono::seconds(secs));
            };
            session.stdio.terminate = [process] { process->terminate(); };
            return session;
        }
        McpEndpoint endpoint;
        endpoint.url          = expand_env_vars(config.url);
        endpoint.bearer_token = expand_env_vars(config.bearer_token);
        for (const auto& [name, value] : config.headers) {
            endpoint.headers.push_back(
                expand_env_vars(name) + ": " + expand_env_vars(value));
        }
        endpoint.timeout_secs = timeout;
        McpSession session;
        session.endpoint = std::move(endpoint);
        return session;
    }

    bool connection_fields_changed(
        const McpServerConfig& old, const McpServerConfig& fresh)
    {
        return old.type != fresh.type || old.url != fresh.url
            || old.catalog_id != fresh.catalog_id
            || old.bearer_token != fresh.bearer_token
            || old.headers != fresh.headers || old.command != fresh.command
            || old.args != fresh.args || old.env != fresh.env
            || old.working_directory != fresh.working_directory
            || old.enabled != fresh.enabled;
    }

} // namespace

McpManager::McpManager(std::map<std::string, McpServerConfig> servers)
{
    reload(std::move(servers));
}

McpManager::~McpManager()
{
    // Workers reference this through the signal and their entries; stop
    // streams and retries, then join before any member dies.
    _stopping.store(true);
    for (auto& [id, entry] : _servers) {
        entry->stream_cancel.store(true);
    }
    // Join in waves: a worker may append follow-up work while we join.
    for (;;) {
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(_workers_mutex);
            workers.swap(_workers);
        }
        if (workers.empty()) {
            break;
        }
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
    for (auto& [id, entry] : _servers) {
        std::lock_guard io_lock(entry->io);
        mcp_end_session(entry->session);
    }
}

void McpManager::reload(std::map<std::string, McpServerConfig> servers)
{
    std::map<std::string, std::shared_ptr<ServerEntry>> fresh;
    bool changed = false;
    {
        std::lock_guard map_lock(_map_mutex);
        for (auto& [id, config] : servers) {
            config.id = id;
            if (config.label.empty() && !config.catalog_id.empty()) {
                if (const auto entry
                    = find_mcp_catalog_entry(config.catalog_id)) {
                    config.label = entry->label;
                }
            }
            if (config.label.empty()) {
                config.label = id;
            }
            const auto found = _servers.find(id);
            if (found == _servers.end()) {
                auto entry    = std::make_shared<ServerEntry>();
                entry->config = std::move(config);
                entry->state  = entry->config.enabled ? McpServerState::OFFLINE
                                                      : McpServerState::DISABLED;
                fresh.emplace(id, std::move(entry));
                changed = true;
                continue;
            }
            std::shared_ptr<ServerEntry> entry = found->second;
            if (connection_fields_changed(entry->config, config)) {
                // Connection changed: drop the session without network
                // I/O; the server expires the abandoned session itself.
                std::lock_guard io_lock(entry->io);
                std::lock_guard state_lock(entry->state_mutex);
                entry->config  = std::move(config);
                entry->session = McpSession { };
                entry->tools.clear();
                entry->detail = "";
                entry->state  = entry->config.enabled ? McpServerState::OFFLINE
                                                      : McpServerState::DISABLED;
                changed       = true;
            } else {
                std::lock_guard state_lock(entry->state_mutex);
                entry->config.label       = config.label;
                entry->config.description = config.description;
            }
            fresh.emplace(id, std::move(entry));
        }
        if (_servers.size() != servers.size()) {
            changed = true; // removals
        }
        _servers = std::move(fresh);
    }
    if (changed) {
        _changed.publish();
    }
}

std::shared_ptr<McpManager::ServerEntry> McpManager::find(
    const std::string& id) const
{
    std::lock_guard map_lock(_map_mutex);
    const auto found = _servers.find(id);
    return found == _servers.end() ? nullptr : found->second;
}

void McpManager::spawn(std::function<void()> work)
{
    std::lock_guard lock(_workers_mutex);
    _workers.emplace_back(std::move(work));
}

void McpManager::run_handshake(std::shared_ptr<ServerEntry> entry)
{
    std::string detail;
    std::optional<McpSession> fresh = make_session(entry->config, detail);
    std::vector<McpToolDefinition> tools;
    bool failed = false;
    int attempt = 0;
    {
        std::lock_guard io_lock(entry->io);
        Status st = Status::OK;
        if (!fresh) {
            st = Status::CONFIG_ERROR;
        } else {
            st = mcp_initialize(*fresh, detail);
            if (st == Status::OK) {
                st = mcp_list_tools(*fresh, tools, detail);
            }
        }
        std::lock_guard state_lock(entry->state_mutex);
        if (st == Status::OK) {
            entry->session            = std::move(*fresh);
            entry->tools              = std::move(tools);
            entry->state              = McpServerState::CONNECTED;
            entry->reconnect_attempts = 0;
        } else {
            failed                    = true;
            entry->reconnect_attempts = std::min(
                entry->reconnect_attempts + 1, MAX_RECONNECT_ATTEMPTS);
            attempt = entry->reconnect_attempts;
            if (fresh) {
                mcp_end_session(*fresh); // a failed handshake leaves a child
            }
            entry->session = McpSession { };
            entry->tools.clear();
            entry->state  = McpServerState::FAILED;
            entry->detail = detail;
        }
    }
    _changed.publish();
    if (failed) {
        if (attempt < MAX_RECONNECT_ATTEMPTS) {
            const long delay
                = RETRY_DELAYS_SECS[static_cast<std::size_t>(attempt - 1)];
            spawn([this, entry, delay] {
                if (wait_interruptible(delay * 1000)) {
                    return;
                }
                run_handshake(entry);
            });
        }
        return;
    }
    // stdio servers push nothing over a GET stream.
    bool stdio = false;
    {
        std::lock_guard state_lock(entry->state_mutex);
        stdio = entry->session.is_stdio();
    }
    if (!stdio) {
        spawn([this, entry] { run_listener(entry); });
    }
}

bool McpManager::wait_interruptible(long ms) const
{
    for (long waited = 0; waited < ms && !_stopping.load(); waited += 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return _stopping.load();
}

void McpManager::run_listener(std::shared_ptr<ServerEntry> entry)
{
    std::string last_event_id;
    long retry_delay_ms = 3000;
    while (!_stopping.load()) {
        {
            std::lock_guard state_lock(entry->state_mutex);
            if (entry->state != McpServerState::CONNECTED
                || entry->session.session_id.empty()) {
                return;
            }
        }
        entry->stream_cancel.store(false);
        std::string stream_url;
        std::vector<std::string> headers;
        {
            // Snapshot the session identity; the GET itself must not hold
            // the I/O mutex (calls and re-lists acquire it in callbacks).
            std::lock_guard io_lock(entry->io);
            stream_url = resolved_url(entry->config);
            headers    = {
                "Accept: text/event-stream",
                "MCP-Session-Id: " + entry->session.session_id,
                "MCP-Protocol-Version: " + entry->session.protocol_version,
            };
            if (!entry->config.bearer_token.empty()) {
                headers.push_back("Authorization: Bearer "
                    + expand_env_vars(entry->config.bearer_token));
            }
            for (const auto& [name, value] : entry->config.headers) {
                headers.push_back(
                    expand_env_vars(name) + ": " + expand_env_vars(value));
            }
        }

        long http_code = 0;
        http_sse_get(
            stream_url, headers, entry->stream_cancel,
            [this, &entry, &last_event_id](const SseEvent& event) {
                if (!event.id.empty()) {
                    last_event_id = std::string(event.id);
                }
                const JsonValue message = parse_json(event.data);
                if (!message.is_object()) {
                    return;
                }
                const JsonValue* method = find_member(message, "method");
                const JsonValue* id     = find_member(message, "id");
                if (method == nullptr || !method->is_string()) {
                    return; // not a request/notification we act on
                }
                const std::string name = method->as<std::string>();
                if (name == "notifications/tools/list_changed"
                    && id == nullptr) {
                    std::vector<McpToolDefinition> tools;
                    std::string detail;
                    {
                        std::lock_guard io_lock(entry->io);
                        const Status st
                            = mcp_list_tools(entry->session, tools, detail);
                        if (st != Status::OK) {
                            return;
                        }
                        std::lock_guard state_lock(entry->state_mutex);
                        entry->tools = std::move(tools);
                    }
                    _changed.publish();
                    return;
                }
                if (name == "ping" && id != nullptr) {
                    std::lock_guard io_lock(entry->io);
                    mcp_send_result(entry->session, *id, JsonValue { });
                }
                // Other server requests are out of scope; leave them
                // unanswered rather than guessing at their semantics.
            },
            last_event_id, &http_code);

        if (_stopping.load()) {
            return;
        }
        if (http_code == 405) {
            // This server offers no server-initiated stream; done for this
            // session (the next handshake tries again).
            return;
        }
        if (http_code == 404) {
            // Session expired: rebuild, and let the fresh handshake spawn
            // the replacement listener. run_handshake takes the entry I/O
            // lock itself; holding it here too would self-deadlock the
            // non-recursive mutex and hang every later call on this entry.
            run_handshake(entry);
            return;
        }
        if (wait_interruptible(retry_delay_ms / 1000 + 1)) {
            return;
        }
    }
}

void McpManager::connect(const std::string& id)
{
    const std::shared_ptr<ServerEntry> entry = find(id);
    if (entry == nullptr) {
        return;
    }
    {
        std::lock_guard state_lock(entry->state_mutex);
        if (!entry->config.enabled || entry->state == McpServerState::CONNECTING
            || entry->state == McpServerState::CONNECTED) {
            return;
        }
        // A dangling catalogue reference fails immediately, offline; it
        // may resolve again after the next app update.
        if (!entry->config.is_stdio() && entry->config.url.empty()
            && resolved_url(entry->config).empty()) {
            entry->state  = McpServerState::FAILED;
            entry->detail = dangling_catalog_detail(entry->config);
            _changed.publish();
            return;
        }
        entry->state  = McpServerState::CONNECTING;
        entry->detail = "";
    }
    _changed.publish();
    spawn([this, entry] { run_handshake(entry); });
}

void McpManager::disconnect(const std::string& id)
{
    const std::shared_ptr<ServerEntry> entry = find(id);
    if (entry == nullptr) {
        return;
    }
    {
        std::lock_guard state_lock(entry->state_mutex);
        if (entry->state != McpServerState::CONNECTED
            && entry->state != McpServerState::FAILED) {
            return;
        }
        entry->state = McpServerState::OFFLINE;
        entry->tools.clear();
        entry->detail = "";
    }
    _changed.publish();
    spawn([entry] {
        std::lock_guard io_lock(entry->io);
        mcp_end_session(entry->session);
    });
}

Status McpManager::call(const std::string& id, const std::string& tool,
    const JsonValue& arguments, McpToolCallResult& out, std::string& detail)
{
    const std::shared_ptr<ServerEntry> entry = find(id);
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
    std::map<std::string, std::shared_ptr<ServerEntry>> entries;
    {
        std::lock_guard map_lock(_map_mutex);
        entries = _servers;
    }
    std::vector<McpServerSnapshot> out;
    out.reserve(entries.size());
    for (const auto& [id, entry] : entries) {
        std::lock_guard state_lock(entry->state_mutex);
        out.push_back(McpServerSnapshot { id, entry->config.label,
            entry->config.description, entry->detail, entry->state,
            entry->tools.size() });
    }
    return out;
}

std::optional<std::vector<McpToolDefinition>> McpManager::tools(
    const std::string& id) const
{
    const std::shared_ptr<ServerEntry> entry = find(id);
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
