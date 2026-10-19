#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/modal.h"
#include "common/permission_request.h"
#include "common/types.h"

namespace imza {

class CredentialStore;

struct Connection {
    std::string id;
    std::string endpoint;
    std::string api_key;
    std::string refresh_token;
    std::int64_t expires_at = 0;
    std::string account_id;
    std::string label;
    std::map<std::string, ApiStandard> dialects;

    bool has_secret() const
    {
        return !api_key.empty() || !refresh_token.empty();
    }

    // A subscription carries two tokens, so they travel together as one JSON
    // object and a refresh updates them atomically; every other connection
    // stores its key alone.
    std::string encode_secret() const;
    bool decode_secret(std::string_view stored);
};

// "id" or "id/label" identity of a connection entry.
inline std::string connection_key_for(
    std::string_view id, std::string_view label)
{
    return label.empty() ? std::string(id)
                         : std::string(id) + "/" + std::string(label);
}
inline std::string connection_key(const Connection& connection)
{
    return connection_key_for(connection.id, connection.label);
}
// First connection whose identity key matches `key`; nullptr when absent.
inline const Connection* find_connection(
    const std::vector<Connection>& connections, std::string_view key)
{
    for (const Connection& connection : connections) {
        if (connection_key(connection) == key) {
            return &connection;
        }
    }
    return nullptr;
}

struct LastUsed {
    std::string provider;
    std::string model;
};

// OAuth credentials captured by a browser sign-in (protocols/mcp_oauth):
// the registered client, the discovered authorization-server endpoints,
// and the current tokens. Plain configuration data, like every other
// credential; the live access token is used as the request bearer.
struct McpOauthCredentials {
    std::string client_id;
    std::string client_secret; // empty for public clients
    std::string issuer;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::string registration_endpoint;
    std::string scopes;
    std::string access_token;
    std::string refresh_token;   // empty when the server issued none
    std::int64_t expires_at = 0; // unix seconds, 0 unknown

    bool has_tokens() const { return !access_token.empty(); }
    bool operator==(const McpOauthCredentials&) const = default;
};

// One user-configured MCP server. A catalogue server stores only
// catalog_id and resolves url/label from the bundled catalogue at use
// time (the models.dev pattern); a custom server stores url directly.
// The url is stored normalized (https).
struct McpServerConfig {
    std::string id;
    std::string label;
    std::string description;
    // "http" (default) or "stdio"; stdio stores command/args/env instead
    // of url/catalog_id and is spawned as a child process.
    std::string type = "http";
    std::string catalog_id;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string bearer_token;
    // OAuth sign-in state for http servers; empty until the first
    // successful browser sign-in.
    McpOauthCredentials oauth;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string working_directory;
    bool enabled  = true;
    bool autoload = false;
    // Per-request timeout override; 0 = the client default (25 s).
    long timeout_secs = 0;

    bool is_stdio() const { return type == "stdio"; }
};

enum class SubagentRole { BUILDER, RESEARCH, BASIC };

struct SubagentModelConfig {
    std::string provider;
    std::string model;
    std::string variant;
};

// Default permission grants installed at startup, before any CLI grants.
struct AllowConfig {
    std::vector<std::filesystem::path> directories;
    std::vector<ShellCommandGrant> commands;
};

// Per-widget visibility for the sidebar. Every widget is shown by default;
// a false flag hides it for the whole session (read once at startup).
struct SidebarConfig {
    bool documents       = true;
    bool review_comments = true;
    bool tasks           = true;
    bool changed_files   = true;
    bool context         = true;
    bool permissions     = true;
};

struct Preferences {
    // Where provider secrets live: the OS keychain, or config.json itself.
    enum class Credentials { OS, PLAINTEXT };
    Credentials credentials = Credentials::OS;
};

struct Config {
    Preferences preferences;
    std::vector<Connection> providers;
    std::optional<LastUsed> last_used;
    std::optional<std::string> reasoning_effort;
    std::map<SubagentRole, SubagentModelConfig> subagents;
    std::map<std::string, SkillPolicy> global_skills;
    std::map<std::string, std::map<std::string, SkillPolicy>> project_skills;
    std::map<std::string, McpServerConfig> mcp_servers;
    AllowConfig allow;
    SidebarConfig sidebar;
    // Extra instruction files loaded after the workspace agent file;
    // absolute or project-root-relative paths.
    std::vector<std::string> instructions;
};

enum class ConfigUpdateResult { UPDATED, UNCHANGED, FAILURE };
using ConfigMutator = std::function<bool(Config&)>;

constexpr std::string_view subagent_default_variant(SubagentRole role)
{
    if (role == SubagentRole::BUILDER) {
        return "medium";
    }
    if (role == SubagentRole::RESEARCH) {
        return "low";
    }
    return "off";
}
Status load_config(const std::filesystem::path& path, Config& out,
    std::string* error = nullptr);
// Secrets are written only when the store has no keychain (plaintext mode).
Status save_config(const std::filesystem::path& path, const Config& cfg,
    CredentialStore* credentials = nullptr);
ConfigUpdateResult update_config(const std::filesystem::path& path,
    const Config& initial, const ConfigMutator& mutate,
    Config* result = nullptr, CredentialStore* credentials = nullptr);

// Fills every connection's api_key/refresh_token from the keychain. An absent
// entry leaves the value parsed from the file, which adopt_legacy_credentials
// then moves.
Status resolve_credentials(Config& cfg, const CredentialStore& credentials);
// Writes changed secrets and drops entries for vanished connections. Runs
// before the document is written, so a keychain-mode save never drops a
// secret it has not stored.
Status sync_credentials(
    const Config& before, const Config& after, CredentialStore& credentials);
// Moves plaintext secrets into the keychain and rewrites the document without
// them. No-op unless the keychain is available; also how a keyring installed
// later, or a switch back to `os`, is picked up. Returns whether the document
// was rewritten.
bool adopt_legacy_credentials(const std::filesystem::path& path, Config& cfg,
    CredentialStore& credentials);
void apply_skill_policies(Config& config, const SkillPolicyChanges& changes);

// Grants declared by `config.allow` are converted for the permission
// store by allow_grants in permissions/permission_store.h.

// Published JSON Schema for config.json, written into the file as
// `$schema` so editors offer autocomplete while hand-editing.
inline constexpr std::string_view CONFIG_SCHEMA_URL
    = "https://imza.run/config.schema.json";

// Imza's own persisted-state root: config, sessions, history, caches.
std::filesystem::path data_dir(void);
inline std::filesystem::path config_path()
{
    return data_dir() / "config.json";
}
inline std::filesystem::path presets_path()
{
    return data_dir() / "presets.json";
}
inline std::filesystem::path sessions_dir() { return data_dir() / "sessions"; }
inline std::filesystem::path input_history_path()
{
    return data_dir() / "input-history.json";
}
inline std::filesystem::path update_state_path()
{
    return data_dir() / "update.json";
}
inline std::filesystem::path prompts_dir() { return data_dir() / "prompts"; }
std::optional<std::string> read_changelog(void);

// Read-only resources shipped with the application: the bundled skills
// tree (e.g. the imza-docs skill) lives beside the changelog in the
// install layout. Empty when the executable path cannot be resolved.
std::filesystem::path bundled_skills_directory(void);

} // namespace imza
