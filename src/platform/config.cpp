#include "platform/config.h"

#include "network/web.h"
#include "permissions/store.h"
#include "platform/file_lock.h"
#include "platform/json_file.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace imza {

std::filesystem::path data_dir()
{
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    return std::filesystem::path(appdata && *appdata ? appdata : ".") / "imza";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : ".") / "Library"
        / "Application Support" / "imza";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return std::filesystem::path(xdg) / "imza";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : ".") / ".local"
        / "share" / "imza";
#endif
}

namespace {
#if defined(_WIN32)
    std::filesystem::path current_executable_path()
    {
        wchar_t buffer[MAX_PATH];
        const DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (size == 0 || size >= MAX_PATH) {
            return { };
        }
        return std::filesystem::path(buffer);
    }
#endif

    std::filesystem::path changelog_path()
    {
#if defined(_WIN32)
        const std::filesystem::path executable = current_executable_path();
        if (executable.empty()) {
            return { };
        }
        return executable.parent_path() / "CHANGELOG.txt";
#else
        return std::filesystem::path { IMZA_INSTALL_PREFIX }
        / IMZA_INSTALL_DOCDIR / "CHANGELOG.txt";
#endif
    }
} // namespace

std::optional<std::string> read_changelog()
{
    const std::filesystem::path path = changelog_path();
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    std::optional<std::string> content = read_text_file(path);
    if (!content || content->empty()) {
        return std::nullopt;
    }
    return content;
}

namespace {

    template <typename Enum, std::size_t N>
    bool parse_enum(const std::array<std::pair<const char*, Enum>, N>& table,
        const std::string& text, Enum& out)
    {
        const auto found = std::find_if(table.begin(), table.end(),
            [&](const auto& entry) { return text == entry.first; });
        if (found == table.end()) {
            return false;
        }
        out = found->second;
        return true;
    }

    template <typename Enum, std::size_t N>
    const char* enum_str(
        const std::array<std::pair<const char*, Enum>, N>& table, Enum value,
        const char* fallback)
    {
        for (const auto& entry : table) {
            if (entry.second == value) {
                return entry.first;
            }
        }
        return fallback;
    }

    constexpr std::array<std::pair<const char*, ApiStandard>, 3> DIALECT_NAMES
        = { {
            { "openai", ApiStandard::OPENAI },
            { "openai-responses", ApiStandard::OPENAI_RESPONSES },
            { "anthropic", ApiStandard::ANTHROPIC },
        } };

    constexpr std::array<std::pair<const char*, SkillPolicy>, 3>
        SKILL_POLICY_NAMES = { {
            { "allow", SkillPolicy::ALLOW },
            { "ask", SkillPolicy::ASK },
            { "deny", SkillPolicy::DENY },
        } };

    bool parse_dialect(const std::string& text, ApiStandard& out)
    {
        return parse_enum(DIALECT_NAMES, text, out);
    }

    const char* dialect_str(ApiStandard standard)
    {
        return enum_str(DIALECT_NAMES, standard, "openai");
    }

    bool parse_skill_policy(const std::string& text, SkillPolicy& out)
    {
        return parse_enum(SKILL_POLICY_NAMES, text, out);
    }

    const char* skill_policy_str(SkillPolicy policy)
    {
        return enum_str(SKILL_POLICY_NAMES, policy, "ask");
    }

} // namespace

// Glaze-reflected: must have external linkage (Clang/MSVC requirement).
// Wire-shaped mirrors of the config document: enums stay strings on
// the wire, so the stored structs carry the names and validation maps
// them after the parse (keeps per-field error messages).
struct StoredConnection {
    std::optional<std::string> id;
    // Legacy key superseded by "id"; used only when "id" is absent.
    std::optional<std::string> provider_id;
    std::optional<std::string> endpoint;
    std::optional<std::string> api_key;
    std::optional<std::string> refresh_token;
    std::optional<std::int64_t> expires_at;
    std::optional<std::string> account_id;
    std::optional<std::string> label;
    std::optional<std::map<std::string, std::string>> dialects;
};

struct StoredModelChoice {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> reasoning_effort;
};

struct StoredModels {
    std::optional<StoredModelChoice> main;
    std::optional<StoredModelChoice> builder;
    std::optional<StoredModelChoice> researcher;
    std::optional<StoredModelChoice> basic;
};

struct StoredSkills {
    std::optional<std::map<std::string, std::string>> global;
    std::optional<std::map<std::string, std::map<std::string, std::string>>>
        projects;
};

struct StoredMcpOauth {
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
    std::optional<std::string> issuer;
    std::optional<std::string> authorization_endpoint;
    std::optional<std::string> token_endpoint;
    std::optional<std::string> registration_endpoint;
    std::optional<std::string> scopes;
    std::optional<std::string> access_token;
    std::optional<std::string> refresh_token;
    std::optional<std::int64_t> expires_at;
};

struct StoredMcpServer {
    std::optional<std::string> label;
    std::optional<std::string> description;
    std::optional<std::string> type;
    std::optional<std::string> catalog_id;
    std::optional<std::string> url;
    std::optional<std::map<std::string, std::string>> headers;
    std::optional<std::string> bearer_token;
    std::optional<StoredMcpOauth> oauth;
    std::optional<std::string> command;
    std::optional<std::vector<std::string>> args;
    std::optional<std::map<std::string, std::string>> env;
    std::optional<std::string> working_directory;
    std::optional<bool> enabled;
    std::optional<bool> autoload;
    std::optional<long> timeout_secs;
};

struct StoredCommandGrant {
    std::optional<std::string> program;
    std::optional<std::string> subcommand;
};

struct StoredAllow {
    std::optional<std::vector<std::string>> directories;
    std::optional<std::vector<StoredCommandGrant>> commands;
};

struct StoredConfig {
    std::optional<std::string> schema_url;
    std::optional<std::vector<StoredConnection>> providers;
    std::optional<StoredModels> models;
    std::optional<StoredSkills> skills;
    std::optional<std::map<std::string, StoredMcpServer>> mcp_servers;
    std::optional<StoredAllow> allow;
    std::optional<std::vector<std::string>> instructions;
};

Status load_config(
    const std::filesystem::path& path, Config& out, std::string* error)
{
    const auto fail = [&](Status st, const std::string& msg) {
        if (error != nullptr) {
            *error = msg;
        }
        return st;
    };

    out = Config { };

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return Status::OK;
    }
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        return fail(Status::CONFIG_ERROR, "failed to read " + path.string());
    }

    StoredConfig stored;
    if (glz::error_ctx error = json_parse_checked(*text, stored)) {
        return fail(Status::CONFIG_ERROR,
            "invalid JSON: " + imza::json_parse_error(*text, error));
    }

    if (stored.providers) {
        for (StoredConnection& entry : *stored.providers) {
            Connection conn;
            conn.id = entry.id.value_or("");
            if (conn.id.empty() && !entry.id.has_value()) {
                conn.id = entry.provider_id.value_or("");
            }
            conn.endpoint      = entry.endpoint.value_or("");
            conn.api_key       = entry.api_key.value_or("");
            conn.refresh_token = entry.refresh_token.value_or("");
            conn.expires_at    = entry.expires_at.value_or(0);
            conn.account_id    = entry.account_id.value_or("");
            conn.label         = entry.label.value_or("");
            if (conn.id.empty()) {
                return fail(Status::CONFIG_ERROR,
                    "provider entry requires a non-empty 'id'");
            }
            if (entry.dialects) {
                for (const auto& [model, name] : *entry.dialects) {
                    ApiStandard standard;
                    if (!parse_dialect(name, standard)) {
                        return fail(Status::CONFIG_ERROR,
                            "unknown dialect for model '" + model + "'");
                    }
                    conn.dialects[model] = standard;
                }
            }
            out.providers.push_back(std::move(conn));
        }
        std::set<std::string> seen;
        for (Connection& connection : out.providers) {
            if (seen.insert(connection_key(connection)).second) {
                continue;
            }
            const std::string base
                = connection.label.empty() ? "" : connection.label + " ";
            int suffix = 2;
            while (true) {
                connection.label = base + std::to_string(suffix);
                if (seen.insert(connection_key(connection)).second) {
                    break;
                }
                ++suffix;
            }
        }
    }

    const auto connection_exists = [&](const std::string& id) {
        return find_connection(out.providers, id) != nullptr;
    };
    if (stored.models && stored.models->main) {
        const StoredModelChoice& main = *stored.models->main;
        const std::string provider    = main.provider.value_or("");
        const std::string model       = main.model.value_or("");
        if (provider.empty() && !model.empty()) {
            return fail(Status::CONFIG_ERROR,
                "'models.main.model' requires a provider");
        }
        if (!provider.empty() && !connection_exists(provider)) {
            return fail(Status::CONFIG_ERROR,
                "'models.main.provider' does not resolve to a connection");
        }
        if (!provider.empty()) {
            out.last_used = LastUsed { provider, model };
        }
        const std::string reasoning = main.reasoning_effort.value_or("");
        out.reasoning_effort        = reasoning.empty()
            ? std::optional<std::string> { }
            : std::optional<std::string> { reasoning };
    }
    if (stored.models) {
        const auto parse_model = [&](std::optional<StoredModelChoice>& value,
                                     SubagentRole role) {
            if (!value) {
                return true;
            }
            SubagentModelConfig config;
            config.provider = value->provider.value_or("");
            config.model    = value->model.value_or("");
            config.variant  = value->reasoning_effort.value_or("");
            if (config.provider.empty() != config.model.empty()) {
                return false;
            }
            if (!config.provider.empty()
                && !connection_exists(config.provider)) {
                return false;
            }
            if (!config.variant.empty()
                && !std::ranges::contains(REASONING_EFFORTS, config.variant)) {
                return false;
            }
            out.subagents[role] = std::move(config);
            return true;
        };
        if (!parse_model(stored.models->builder, SubagentRole::BUILDER)
            || !parse_model(stored.models->researcher, SubagentRole::RESEARCH)
            || !parse_model(stored.models->basic, SubagentRole::BASIC)) {
            return fail(Status::CONFIG_ERROR, "invalid models configuration");
        }
    }

    if (stored.skills) {
        const auto parse_map
            = [&](const std::map<std::string, std::string>* value,
                  std::map<std::string, SkillPolicy>& target) {
                  if (value == nullptr) {
                      return true;
                  }
                  for (const auto& [name, policy_name] : *value) {
                      SkillPolicy policy;
                      if (!parse_skill_policy(policy_name, policy)) {
                          return false;
                      }
                      target[name] = policy;
                  }
                  return true;
              };
        if (!parse_map(
                stored.skills->global ? &*stored.skills->global : nullptr,
                out.global_skills)) {
            return fail(Status::CONFIG_ERROR, "invalid global skill policy");
        }
        if (stored.skills->projects) {
            for (auto& [project_path, policies] : *stored.skills->projects) {
                if (!parse_map(&policies, out.project_skills[project_path])) {
                    return fail(
                        Status::CONFIG_ERROR, "invalid project skill policy");
                }
            }
        }
    }

    if (stored.mcp_servers) {
        for (auto& [id, entry] : *stored.mcp_servers) {
            McpServerConfig server;
            server.id           = id;
            server.label        = entry.label.value_or("");
            server.description  = entry.description.value_or("");
            server.bearer_token = entry.bearer_token.value_or("");
            server.enabled      = entry.enabled.value_or(true);
            server.autoload     = entry.autoload.value_or(false);
            server.timeout_secs = entry.timeout_secs.value_or(0);
            server.type         = entry.type.value_or("http");
            if (server.type != "http" && server.type != "stdio") {
                return fail(Status::CONFIG_ERROR,
                    "mcp server '" + id + "': unknown type '" + server.type
                        + "'");
            }
            if (server.timeout_secs < 0) {
                return fail(Status::CONFIG_ERROR,
                    "mcp server '" + id + "': negative timeout");
            }
            if (entry.headers) {
                for (const auto& [name, value] : *entry.headers) {
                    if (name.empty() || name.contains(':')) {
                        return fail(Status::CONFIG_ERROR,
                            "mcp server '" + id + "': invalid header name '"
                                + name + "'");
                    }
                    server.headers[name] = value;
                }
            }
            if (entry.oauth) {
                if (server.is_stdio()) {
                    return fail(Status::CONFIG_ERROR,
                        "mcp server '" + id
                            + "': oauth is only valid for http servers");
                }
                McpOauthCredentials& oauth = server.oauth;
                oauth.client_id     = entry.oauth->client_id.value_or("");
                oauth.client_secret = entry.oauth->client_secret.value_or("");
                oauth.issuer        = entry.oauth->issuer.value_or("");
                oauth.authorization_endpoint
                    = entry.oauth->authorization_endpoint.value_or("");
                oauth.token_endpoint = entry.oauth->token_endpoint.value_or("");
                oauth.registration_endpoint
                    = entry.oauth->registration_endpoint.value_or("");
                oauth.scopes        = entry.oauth->scopes.value_or("");
                oauth.access_token  = entry.oauth->access_token.value_or("");
                oauth.refresh_token = entry.oauth->refresh_token.value_or("");
                oauth.expires_at    = entry.oauth->expires_at.value_or(0);
                const char* endpoints[] = { "issuer", "authorization_endpoint",
                    "token_endpoint", "registration_endpoint" };
                const std::string values[]
                    = { oauth.issuer, oauth.authorization_endpoint,
                          oauth.token_endpoint, oauth.registration_endpoint };
                for (std::size_t at = 0; at < 4; ++at) {
                    const std::string& url = values[at];
                    const bool ok = url.empty() || url.starts_with("https://")
                        || url.starts_with("http://127.0.0.1")
                        || url.starts_with("http://localhost")
                        || url.starts_with("http://[::1]");
                    if (!ok) {
                        return fail(Status::CONFIG_ERROR,
                            "mcp server '" + id + "': oauth " + endpoints[at]
                                + " must be https");
                    }
                }
            }
            if (server.is_stdio()) {
                server.command = entry.command.value_or("");
                if (server.command.empty()) {
                    return fail(Status::CONFIG_ERROR,
                        "mcp server '" + id
                            + "' requires a command for the stdio transport");
                }
                if (entry.args) {
                    server.args = *entry.args;
                }
                if (entry.env) {
                    server.env = *entry.env;
                }
                server.working_directory = entry.working_directory.value_or("");
                out.mcp_servers[id]      = std::move(server);
                continue;
            }
            // A catalogue reference or an explicit url; both absent is
            // unusable. An explicit url wins over the reference.
            server.catalog_id         = entry.catalog_id.value_or("");
            const std::string raw_url = entry.url.value_or("");
            if (!raw_url.empty()) {
                std::string url;
                if (normalize_web_url(raw_url, url) != Status::OK) {
                    return fail(Status::CONFIG_ERROR,
                        "mcp server '" + id + "' requires an http(s) url");
                }
                server.url = std::move(url);
            } else if (server.catalog_id.empty()) {
                return fail(Status::CONFIG_ERROR,
                    "mcp server '" + id
                        + "' requires an http(s) url or a catalog id");
            }
            out.mcp_servers[id] = std::move(server);
        }
    }

    if (stored.allow) {
        if (stored.allow->directories) {
            for (const std::string& entry : *stored.allow->directories) {
                if (entry.empty()) {
                    return fail(Status::CONFIG_ERROR,
                        "'allow.directories' entry must not be empty");
                }
                out.allow.directories.emplace_back(entry);
            }
        }
        if (stored.allow->commands) {
            for (const StoredCommandGrant& entry : *stored.allow->commands) {
                ShellCommandGrant grant;
                grant.program = entry.program.value_or("");
                if (grant.program.empty()) {
                    return fail(Status::CONFIG_ERROR,
                        "'allow.commands' entry requires a non-empty"
                        " 'program'");
                }
                if (entry.subcommand.has_value()) {
                    if (entry.subcommand->empty()) {
                        return fail(Status::CONFIG_ERROR,
                            "'allow.commands' entry for '" + grant.program
                                + "' has an empty 'subcommand'");
                    }
                    grant.subcommand = entry.subcommand;
                }
                out.allow.commands.push_back(std::move(grant));
            }
        }
    }

    if (stored.instructions) {
        for (const std::string& entry : *stored.instructions) {
            if (entry.empty()) {
                return fail(Status::CONFIG_ERROR,
                    "'instructions' entry must not be empty");
            }
            out.instructions.push_back(entry);
        }
    }

    return Status::OK;
}

PermissionStore::Grants allow_grants(const AllowConfig& allow)
{
    PermissionStore::Grants grants;
    grants.reserve(allow.directories.size() + allow.commands.size());
    std::error_code ec;
    for (const std::filesystem::path& entry : allow.directories) {
        const std::filesystem::path resolved = entry.is_absolute()
            ? entry
            : std::filesystem::weakly_canonical(
                  std::filesystem::current_path() / entry, ec);
        if (ec || !std::filesystem::is_directory(resolved, ec) || ec) {
            continue;
        }
        grants.emplace_back(resolved);
    }
    for (const ShellCommandGrant& command : allow.commands) {
        grants.emplace_back(command);
    }
    return grants;
}

void apply_skill_policies(Config& config, const SkillPolicyChanges& changes)
{
    for (const auto& entry : changes.entries) {
        if (entry.project_root.empty())
            config.global_skills[entry.name] = entry.policy;
        else
            config.project_skills[entry.project_root][entry.name]
                = entry.policy;
    }
}

namespace {

    Status save_config_unlocked(
        const std::filesystem::path& path, const Config& cfg)
    {
        StoredConfig stored;
        stored.schema_url = std::string(CONFIG_SCHEMA_URL);
        std::vector<StoredConnection> providers;
        providers.reserve(cfg.providers.size());
        for (const Connection& conn : cfg.providers) {
            StoredConnection entry;
            entry.id = conn.id;
            if (!conn.endpoint.empty()) {
                entry.endpoint = conn.endpoint;
            }
            if (!conn.api_key.empty()) {
                entry.api_key = conn.api_key;
            }
            if (!conn.refresh_token.empty()) {
                entry.refresh_token = conn.refresh_token;
            }
            if (conn.expires_at != 0) {
                entry.expires_at = conn.expires_at;
            }
            if (!conn.account_id.empty()) {
                entry.account_id = conn.account_id;
            }
            if (!conn.label.empty()) {
                entry.label = conn.label;
            }
            if (!conn.dialects.empty()) {
                std::map<std::string, std::string> dialects;
                for (const auto& [model, standard] : conn.dialects) {
                    dialects[model] = dialect_str(standard);
                }
                entry.dialects = std::move(dialects);
            }
            providers.push_back(std::move(entry));
        }
        stored.providers = std::move(providers);

        StoredModels models;
        StoredModelChoice main;
        if (cfg.last_used) {
            main.provider = cfg.last_used->provider;
            main.model    = cfg.last_used->model;
        }
        main.reasoning_effort     = cfg.reasoning_effort.value_or("off");
        models.main               = std::move(main);
        const auto write_subagent = [&](SubagentRole role) {
            StoredModelChoice value;
            const auto found = cfg.subagents.find(role);
            if (found != cfg.subagents.end()
                && !found->second.provider.empty()) {
                value.provider = found->second.provider;
                value.model    = found->second.model;
            }
            value.reasoning_effort = to_config_effort(
                found == cfg.subagents.end() || found->second.variant.empty()
                    ? subagent_default_variant(role)
                    : found->second.variant);
            return value;
        };
        models.builder    = write_subagent(SubagentRole::BUILDER);
        models.researcher = write_subagent(SubagentRole::RESEARCH);
        models.basic      = write_subagent(SubagentRole::BASIC);
        stored.models     = std::move(models);

        StoredSkills skills;
        std::map<std::string, std::string> global;
        for (const auto& [name, policy] : cfg.global_skills) {
            global[name] = skill_policy_str(policy);
        }
        skills.global = std::move(global);
        std::map<std::string, std::map<std::string, std::string>> projects;
        for (const auto& [project_path, policies] : cfg.project_skills) {
            std::map<std::string, std::string> entry;
            for (const auto& [name, policy] : policies) {
                entry[name] = skill_policy_str(policy);
            }
            projects[project_path] = std::move(entry);
        }
        skills.projects = std::move(projects);
        stored.skills   = std::move(skills);

        if (!cfg.mcp_servers.empty()) {
            std::map<std::string, StoredMcpServer> servers;
            for (const auto& [id, server] : cfg.mcp_servers) {
                StoredMcpServer entry;
                if (!server.label.empty()) {
                    entry.label = server.label;
                }
                if (!server.description.empty()) {
                    entry.description = server.description;
                }
                if (server.type != "http") {
                    entry.type = server.type;
                }
                if (!server.catalog_id.empty()) {
                    entry.catalog_id = server.catalog_id;
                }
                if (!server.url.empty()) {
                    entry.url = server.url;
                }
                if (!server.headers.empty()) {
                    entry.headers = server.headers;
                }
                if (!server.bearer_token.empty()) {
                    entry.bearer_token = server.bearer_token;
                }
                if (server.oauth.has_tokens()
                    || !server.oauth.client_id.empty()) {
                    StoredMcpOauth oauth;
                    if (!server.oauth.client_id.empty()) {
                        oauth.client_id = server.oauth.client_id;
                    }
                    if (!server.oauth.client_secret.empty()) {
                        oauth.client_secret = server.oauth.client_secret;
                    }
                    if (!server.oauth.issuer.empty()) {
                        oauth.issuer = server.oauth.issuer;
                    }
                    if (!server.oauth.authorization_endpoint.empty()) {
                        oauth.authorization_endpoint
                            = server.oauth.authorization_endpoint;
                    }
                    if (!server.oauth.token_endpoint.empty()) {
                        oauth.token_endpoint = server.oauth.token_endpoint;
                    }
                    if (!server.oauth.registration_endpoint.empty()) {
                        oauth.registration_endpoint
                            = server.oauth.registration_endpoint;
                    }
                    if (!server.oauth.scopes.empty()) {
                        oauth.scopes = server.oauth.scopes;
                    }
                    if (!server.oauth.access_token.empty()) {
                        oauth.access_token = server.oauth.access_token;
                    }
                    if (!server.oauth.refresh_token.empty()) {
                        oauth.refresh_token = server.oauth.refresh_token;
                    }
                    if (server.oauth.expires_at > 0) {
                        oauth.expires_at = server.oauth.expires_at;
                    }
                    entry.oauth = std::move(oauth);
                }
                if (!server.command.empty()) {
                    entry.command = server.command;
                }
                if (!server.args.empty()) {
                    entry.args = server.args;
                }
                if (!server.env.empty()) {
                    entry.env = server.env;
                }
                if (!server.working_directory.empty()) {
                    entry.working_directory = server.working_directory;
                }
                if (!server.enabled) {
                    entry.enabled = server.enabled;
                }
                if (server.autoload) {
                    entry.autoload = server.autoload;
                }
                if (server.timeout_secs > 0) {
                    entry.timeout_secs = server.timeout_secs;
                }
                servers[id] = std::move(entry);
            }
            stored.mcp_servers = std::move(servers);
        }

        if (!cfg.allow.directories.empty() || !cfg.allow.commands.empty()) {
            StoredAllow allow;
            if (!cfg.allow.directories.empty()) {
                std::vector<std::string> directories;
                for (const std::filesystem::path& entry :
                    cfg.allow.directories) {
                    directories.push_back(entry.string());
                }
                allow.directories = std::move(directories);
            }
            if (!cfg.allow.commands.empty()) {
                std::vector<StoredCommandGrant> commands;
                for (const ShellCommandGrant& entry : cfg.allow.commands) {
                    StoredCommandGrant command;
                    command.program = entry.program;
                    if (entry.subcommand) {
                        command.subcommand = entry.subcommand;
                    }
                    commands.push_back(std::move(command));
                }
                allow.commands = std::move(commands);
            }
            stored.allow = std::move(allow);
        }

        if (!cfg.instructions.empty()) {
            stored.instructions = cfg.instructions;
        }

        auto serialized = json_dump_pretty_checked(stored);
        if (!serialized) {
            return Status::JSON_ERROR;
        }
        return write_json_file(path, *serialized);
    }

} // namespace

Status save_config(const std::filesystem::path& path, const Config& cfg)
{
    auto lock = acquire_file_lock(lock_path_for(path));
    if (!std::holds_alternative<FileLock>(lock)) {
        return Status::CONFIG_ERROR;
    }
    return save_config_unlocked(path, cfg);
}

ConfigUpdateResult update_config(const std::filesystem::path& path,
    const Config& initial, const ConfigMutator& mutate, Config* result)
{
    auto lock = acquire_file_lock(lock_path_for(path));
    if (!std::holds_alternative<FileLock>(lock)) {
        return ConfigUpdateResult::FAILURE;
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return ConfigUpdateResult::FAILURE;
    }
    Config candidate = initial;
    if (exists && load_config(path, candidate) != Status::OK) {
        return ConfigUpdateResult::FAILURE;
    }
    if (!mutate(candidate)) {
        if (result != nullptr) {
            *result = std::move(candidate);
        }
        return ConfigUpdateResult::UNCHANGED;
    }
    if (save_config_unlocked(path, candidate) != Status::OK) {
        return ConfigUpdateResult::FAILURE;
    }
    if (result != nullptr) {
        *result = std::move(candidate);
    }
    return ConfigUpdateResult::UPDATED;
}

} // namespace imza

template <> struct glz::meta<imza::StoredConfig> {
    using T                     = imza::StoredConfig;
    static constexpr auto value = glz::object("$schema", &T::schema_url,
        "providers", &T::providers, "models", &T::models, "skills", &T::skills,
        "mcp_servers", &T::mcp_servers, "allow", &T::allow, "instructions",
        &T::instructions);
};
