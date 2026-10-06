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
#include "common/types.h"
#include "permissions/store.h"

namespace imza {

struct Connection {
    std::string id;
    std::string endpoint;
    std::string api_key;
    std::string refresh_token;
    std::int64_t expires_at = 0;
    std::string account_id;
    std::string label;
    std::map<std::string, ApiStandard> dialects;
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

// One user-configured MCP server. A catalogue server stores only
// catalog_id and resolves url/label from the bundled catalogue at use
// time (the models.dev pattern); a custom server stores url directly.
// stdio transport and OAuth credentials will extend this record later;
// the url is stored normalized (https).
struct McpServerConfig {
    std::string id;
    std::string label;
    std::string description;
    std::string catalog_id;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string bearer_token;
    bool enabled = true;
    // Per-request timeout override; 0 = the client default (25 s).
    long timeout_secs = 0;
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

struct Config {
    std::vector<Connection> providers;
    std::optional<LastUsed> last_used;
    std::optional<std::string> reasoning_effort;
    std::map<SubagentRole, SubagentModelConfig> subagents;
    std::map<std::string, SkillPolicy> global_skills;
    std::map<std::string, std::map<std::string, SkillPolicy>> project_skills;
    std::map<std::string, McpServerConfig> mcp_servers;
    AllowConfig allow;
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
Status save_config(const std::filesystem::path& path, const Config& cfg);
ConfigUpdateResult update_config(const std::filesystem::path& path,
    const Config& initial, const ConfigMutator& mutate,
    Config* result = nullptr);
void apply_skill_policies(Config& config, const SkillPolicyChanges& changes);

// Grants declared by `config.allow`, ready for PermissionStore::install.
// Directories that no longer exist are dropped rather than failing the
// batch: a stale allowlist entry must not block startup.
PermissionStore::Grants allow_grants(const AllowConfig& allow);

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

} // namespace imza
