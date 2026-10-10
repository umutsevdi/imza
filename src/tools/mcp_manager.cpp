#include "tools/mcp_manager.h"

#include "network/json_io.h"
#include "network/mcp_oauth.h"
#include "platform/process.h"
#include "tools/mcp_catalog.h"

#include <ctime>
#include <utility>

namespace imza {

namespace {

    constexpr long DEFAULT_TIMEOUT_SECS = 25;
    // Reconnect backoff after a failed handshake; 5 attempts then the
    // server stays FAILED until a manual connect resets the count.
    constexpr int MAX_RECONNECT_ATTEMPTS = 5;
    constexpr long RETRY_DELAYS_SECS[]   = { 2, 5, 10, 30, 60 };
    // Refresh the access token this many seconds before it expires.
    constexpr std::int64_t TOKEN_REFRESH_MARGIN_SECS = 300;

    std::string dangling_catalog_detail(const McpServerConfig& config)
    {
        return "catalogue entry '" + config.catalog_id + "' no longer exists";
    }

    // True when the entry holds everything a refresh needs.
    bool refreshable(const McpServerConfig& config)
    {
        return config.oauth.has_tokens() && !config.oauth.token_endpoint.empty()
            && !config.oauth.refresh_token.empty();
    }

    bool entry_is_oauth(const McpServerConfig& config)
    {
        if (config.oauth.has_tokens() || !config.oauth.client_id.empty()) {
            return true;
        }
        if (config.catalog_id.empty()) {
            return false;
        }
        const std::optional<McpCatalogEntry> entry
            = find_mcp_catalog_entry(config.catalog_id);
        return entry.has_value() && entry->auth_kind == "oauth";
    }

    // The bearer the request paths use: the OAuth access token when a
    // sign-in happened, the static configured token otherwise.
    std::string bearer_for(const McpServerConfig& config)
    {
        const std::string& token = !config.oauth.access_token.empty()
            ? config.oauth.access_token
            : config.bearer_token;
        return expand_env_vars(token);
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
        endpoint.url = resolved_mcp_url(config);
        if (endpoint.url.empty()) {
            detail = dangling_catalog_detail(config);
            return std::nullopt;
        }
        endpoint.bearer_token = bearer_for(config);
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
            || old.oauth != fresh.oauth || old.headers != fresh.headers
            || old.command != fresh.command || old.args != fresh.args
            || old.env != fresh.env
            || old.working_directory != fresh.working_directory
            || old.enabled != fresh.enabled;
    }

} // namespace

McpManager::McpManager(
    std::map<std::string, McpServerConfig> servers, McpManagerHooks hooks)
    : _hooks(std::move(hooks))
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
                                                      : McpServerState::INACTIVE;
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
                                                      : McpServerState::INACTIVE;
                changed       = true;
            } else {
                std::lock_guard state_lock(entry->state_mutex);
                entry->config.label       = config.label;
                entry->config.description = config.description;
                entry->config.autoload    = config.autoload;
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

void McpManager::run_handshake(
    std::shared_ptr<ServerEntry> entry, bool force_token_refresh)
{
    // Renew an expiring OAuth token before building the session; a failed
    // refresh needs a fresh sign-in, so retrying the handshake would only
    // hammer the token endpoint.
    bool needs_refresh = false;
    {
        std::lock_guard state_lock(entry->state_mutex);
        needs_refresh = refreshable(entry->config);
    }
    if (needs_refresh) {
        std::string refresh_detail;
        if (!refresh_entry_tokens(entry, force_token_refresh, refresh_detail)) {
            {
                std::lock_guard state_lock(entry->state_mutex);
                entry->session = McpSession { };
                entry->tools.clear();
                entry->state  = McpServerState::FAILED;
                entry->detail = refresh_detail;
            }
            _changed.publish();
            return;
        }
    }

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
            stream_url = resolved_mcp_url(entry->config);
            headers    = {
                "Accept: text/event-stream",
                "MCP-Session-Id: " + entry->session.session_id,
                "MCP-Protocol-Version: " + entry->session.protocol_version,
            };
            const std::string bearer = bearer_for(entry->config);
            if (!bearer.empty()) {
                headers.push_back("Authorization: Bearer " + bearer);
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
        if (http_code == 404 || http_code == 401) {
            // Session expired, or the access token was rejected: rebuild.
            // 401 forces a token refresh so the fresh handshake does not
            // reuse the stale bearer. run_handshake takes the entry I/O
            // lock itself; holding it here too would self-deadlock the
            // non-recursive mutex and hang every later call on this entry.
            run_handshake(entry, http_code == 401);
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
            && resolved_mcp_url(entry->config).empty()) {
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

void McpManager::autoload()
{
    std::vector<std::string> ids;
    {
        std::lock_guard map_lock(_map_mutex);
        for (const auto& [id, entry] : _servers) {
            if (entry->config.autoload) {
                ids.push_back(id);
            }
        }
    }
    for (const std::string& id : ids) {
        connect(id);
    }
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

void McpManager::sign_in(const std::string& id)
{
    const std::shared_ptr<ServerEntry> entry = find(id);
    if (entry == nullptr) {
        return;
    }
    {
        std::lock_guard state_lock(entry->state_mutex);
        if (entry->config.is_stdio()
            || entry->sign_in == McpSignInStatus::RUNNING) {
            return;
        }
        entry->sign_in            = McpSignInStatus::RUNNING;
        entry->sign_in_detail     = "starting";
        entry->reconnect_attempts = 0;
    }
    entry->sign_in_cancel.store(false);
    _changed.publish();
    spawn([this, entry] { run_sign_in(entry); });
}

void McpManager::cancel_sign_in(const std::string& id)
{
    const std::shared_ptr<ServerEntry> entry = find(id);
    if (entry == nullptr) {
        return;
    }
    entry->sign_in_cancel.store(true);
}

void McpManager::run_sign_in(std::shared_ptr<ServerEntry> entry)
{
    const std::string id = [&entry] {
        std::lock_guard state_lock(entry->state_mutex);
        return entry->config.id;
    }();

    McpSignInHooks hooks;
    hooks.open_browser = [this](const std::string& url) {
        return _hooks.open_browser && _hooks.open_browser(url);
    };
    hooks.progress = [this, &entry](std::string_view phase) {
        {
            std::lock_guard state_lock(entry->state_mutex);
            entry->sign_in_detail = std::string(phase);
        }
        _changed.publish();
    };

    std::string url;
    bool has_client = false;
    McpOauthClient existing;
    {
        std::lock_guard state_lock(entry->state_mutex);
        url = resolved_mcp_url(entry->config);
        if (!entry->config.oauth.client_id.empty()) {
            existing.client_id     = entry->config.oauth.client_id;
            existing.client_secret = entry->config.oauth.client_secret;
            has_client             = true;
        }
    }

    const McpSignInOutcome outcome = url.empty()
        ? McpSignInOutcome { .status = Status::CONFIG_ERROR,
              .detail                = dangling_catalog_detail(entry->config) }
        : mcp_oauth_sign_in(url, has_client ? &existing : nullptr, hooks,
              entry->sign_in_cancel);

    bool succeeded = outcome.status == Status::OK;
    {
        std::lock_guard state_lock(entry->state_mutex);
        if (succeeded) {
            McpOauthCredentials& oauth   = entry->config.oauth;
            oauth.client_id              = outcome.client.client_id;
            oauth.client_secret          = outcome.client.client_secret;
            oauth.issuer                 = outcome.as.issuer.value_or("");
            oauth.authorization_endpoint = outcome.as.authorization_endpoint;
            oauth.token_endpoint         = outcome.as.token_endpoint;
            oauth.registration_endpoint
                = outcome.as.registration_endpoint.value_or("");
            std::string scopes;
            if (outcome.as.scopes_supported) {
                for (const std::string& scope : *outcome.as.scopes_supported) {
                    if (!scopes.empty()) {
                        scopes += ' ';
                    }
                    scopes += scope;
                }
            }
            oauth.scopes          = std::move(scopes);
            oauth.access_token    = outcome.tokens.access_token;
            oauth.refresh_token   = outcome.tokens.refresh_token;
            oauth.expires_at      = outcome.tokens.expires_at;
            entry->config.enabled = true;
            entry->sign_in        = McpSignInStatus::NONE;
            entry->sign_in_detail.clear();
        } else {
            entry->sign_in        = McpSignInStatus::FAILED;
            entry->sign_in_detail = outcome.detail;
            // A timeout means the browser never came back: surface the
            // authorize URL so the user can open it by hand.
            if (outcome.status == Status::TIMEOUT
                && !outcome.authorize_url.empty()) {
                entry->sign_in_detail
                    += "\nopen by hand: " + outcome.authorize_url;
            }
        }
    }
    _changed.publish();
    if (!succeeded || _stopping.load()) {
        return;
    }
    // Persist only while the entry is still configured: a removal that
    // raced the sign-in must not resurrect the server in config.json.
    if (!still_tracked(id, entry)) {
        return;
    }
    if (_hooks.persist_server) {
        McpServerConfig persisted;
        {
            std::lock_guard state_lock(entry->state_mutex);
            persisted = entry->config;
        }
        _hooks.persist_server(persisted);
    }
    {
        std::lock_guard state_lock(entry->state_mutex);
        entry->state  = McpServerState::CONNECTING;
        entry->detail = "";
    }
    _changed.publish();
    run_handshake(entry);
}

bool McpManager::still_tracked(
    const std::string& id, const std::shared_ptr<ServerEntry>& entry) const
{
    std::lock_guard map_lock(_map_mutex);
    const auto found = _servers.find(id);
    return found != _servers.end() && found->second == entry;
}

bool McpManager::refresh_entry_tokens(
    const std::shared_ptr<ServerEntry>& entry, bool forced, std::string& detail)
{
    McpOauthCredentials oauth;
    {
        std::lock_guard state_lock(entry->state_mutex);
        oauth = entry->config.oauth;
    }
    if (!forced
        && (oauth.expires_at <= 0
            || oauth.expires_at
                > std::time(nullptr) + TOKEN_REFRESH_MARGIN_SECS)) {
        return true; // still inside its validity window
    }
    if (oauth.token_endpoint.empty() || oauth.refresh_token.empty()) {
        detail = "the access token was rejected and cannot be renewed";
        return false;
    }
    const McpOauthClient client { oauth.client_id, oauth.client_secret };
    McpOauthTokens tokens;
    const Status status = mcp_refresh_tokens(
        oauth.token_endpoint, client, oauth.refresh_token, { }, tokens, detail);
    if (status != Status::OK) {
        detail = "sign-in needed again: " + detail;
        return false;
    }
    {
        std::lock_guard state_lock(entry->state_mutex);
        entry->config.oauth.access_token = tokens.access_token;
        if (!tokens.refresh_token.empty()) {
            entry->config.oauth.refresh_token = tokens.refresh_token;
        }
        entry->config.oauth.expires_at = tokens.expires_at;
    }
    if (_hooks.persist_server && !_stopping.load()
        && still_tracked(entry->config.id, entry)) {
        _hooks.persist_server(entry->config);
    }
    return true;
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
            detail = entry->state == McpServerState::INACTIVE
                ? "mcp server '" + id + "' is inactive"
                : "mcp server '" + id + "' is not connected";
            return Status::CONFIG_ERROR;
        }
    }
    const Status status
        = mcp_call_tool(entry->session, tool, arguments, out, detail);
    // A rejected OAuth access token gets one refresh-and-retry; the entry
    // I/O lock already serializes refreshes on this server. Static-token
    // servers surface the plain 401.
    bool can_refresh = false;
    {
        std::lock_guard state_lock(entry->state_mutex);
        can_refresh = refreshable(entry->config);
    }
    if (status == Status::API_ERROR && can_refresh
        && entry->session.last_http_status == 401) {
        std::string refresh_detail;
        if (refresh_entry_tokens(entry, /*forced=*/true, refresh_detail)) {
            std::string bearer;
            {
                std::lock_guard state_lock(entry->state_mutex);
                bearer = entry->config.oauth.access_token;
            }
            entry->session.endpoint.bearer_token = bearer;
            detail.clear();
            return mcp_call_tool(entry->session, tool, arguments, out, detail);
        }
        detail = refresh_detail;
        return Status::API_ERROR;
    }
    return status;
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
        McpServerSnapshot snapshot { id, entry->config.label,
            entry->config.description, entry->detail, entry->state,
            entry->tools.size(), entry->config.autoload };
        snapshot.oauth          = entry_is_oauth(entry->config);
        snapshot.sign_in        = entry->sign_in;
        snapshot.sign_in_detail = entry->sign_in_detail;
        out.push_back(std::move(snapshot));
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
