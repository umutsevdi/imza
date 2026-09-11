#include "platform/config.h"

#include "platform/file_lock.h"
#include "platform/json_file.h"

#include <json/json.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace imza {

std::filesystem::path config_path() { return data_dir() / "config.json"; }

std::filesystem::path presets_path() { return data_dir() / "presets.json"; }

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

std::filesystem::path sessions_dir() { return data_dir() / "sessions"; }

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
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string content { std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>() };
    return content.empty() ? std::nullopt
                           : std::optional<std::string> { std::move(content) };
}

std::string_view subagent_default_variant(SubagentRole role)
{
    if (role == SubagentRole::BUILDER) {
        return "medium";
    }
    if (role == SubagentRole::RESEARCH) {
        return "low";
    }
    return "off";
}

std::string connection_key(const Connection& connection)
{
    if (connection.label.empty()) {
        return connection.id;
    }
    return connection.id + "/" + connection.label;
}

namespace {

    std::string string_or_empty(const Json::Value& parent, const char* key)
    {
        const Json::Value& v = parent[key];
        return v.isString() ? v.asString() : std::string();
    }

    bool parse_dialect(const std::string& text, ApiStandard& out)
    {
        if (text == "openai") {
            out = ApiStandard::OPENAI;
            return true;
        }
        if (text == "openai-responses") {
            out = ApiStandard::OPENAI_RESPONSES;
            return true;
        }
        if (text == "anthropic") {
            out = ApiStandard::ANTHROPIC;
            return true;
        }
        return false;
    }

    const char* dialect_str(ApiStandard standard)
    {
        switch (standard) {
        case ApiStandard::OPENAI: return "openai";
        case ApiStandard::OPENAI_RESPONSES: return "openai-responses";
        case ApiStandard::ANTHROPIC: return "anthropic";
        }
        return "openai";
    }

    bool parse_skill_policy(const std::string& text, SkillPolicy& out)
    {
        if (text == "allow")
            out = SkillPolicy::ALLOW;
        else if (text == "ask")
            out = SkillPolicy::ASK;
        else if (text == "deny")
            out = SkillPolicy::DENY;
        else
            return false;
        return true;
    }

    const char* skill_policy_str(SkillPolicy policy)
    {
        switch (policy) {
        case SkillPolicy::ALLOW: return "allow";
        case SkillPolicy::ASK: return "ask";
        case SkillPolicy::DENY: return "deny";
        }
        return "ask";
    }

} // namespace

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

    std::ifstream file(path);
    if (!file) {
        return Status::OK;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    if (file.bad() && !file.eof()) {
        return fail(Status::CONFIG_ERROR, "failed to read " + path.string());
    }
    const std::string text = buffer.str();

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string err;
    std::istringstream parse_stream { text };
    if (!Json::parseFromStream(reader, parse_stream, &root, &err)) {
        return fail(Status::CONFIG_ERROR, "invalid JSON: " + err);
    }
    if (!root.isObject()) {
        return fail(Status::CONFIG_ERROR, "config root is not an object");
    }

    const Json::Value& providers = root["providers"];
    if (!providers.isNull()) {
        if (!providers.isArray()) {
            return fail(Status::CONFIG_ERROR, "'providers' must be an array");
        }
        for (const Json::Value& entry : providers) {
            if (!entry.isObject()) {
                return fail(
                    Status::CONFIG_ERROR, "provider entry must be an object");
            }
            Connection conn;
            conn.id = string_or_empty(entry, "id");
            if (conn.id.empty() && !entry.isMember("id")) {
                conn.id = string_or_empty(entry, "provider_id");
            }
            conn.endpoint      = string_or_empty(entry, "endpoint");
            conn.api_key       = string_or_empty(entry, "api_key");
            conn.refresh_token = string_or_empty(entry, "refresh_token");
            if (!entry["expires_at"].isNull()) {
                if (!entry["expires_at"].isInt64()) {
                    return fail(Status::CONFIG_ERROR,
                        "'expires_at' must be an integer");
                }
                conn.expires_at = entry["expires_at"].asInt64();
            }
            conn.account_id = string_or_empty(entry, "account_id");
            conn.label      = string_or_empty(entry, "label");
            if (conn.id.empty()) {
                return fail(Status::CONFIG_ERROR,
                    "provider entry requires a non-empty 'id'");
            }
            const Json::Value& dialects = entry["dialects"];
            if (!dialects.isNull()) {
                if (!dialects.isObject()) {
                    return fail(
                        Status::CONFIG_ERROR, "'dialects' must be an object");
                }
                for (const std::string& model : dialects.getMemberNames()) {
                    ApiStandard standard;
                    if (!parse_dialect(dialects[model].asString(), standard)) {
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

    const Json::Value& models = root["models"];
    if (!models.isNull()) {
        if (!models.isObject()) {
            return fail(Status::CONFIG_ERROR, "'models' must be an object");
        }
        const auto connection_exists = [&](const std::string& id) {
            return std::any_of(out.providers.begin(), out.providers.end(),
                [&](const Connection& connection) {
                    return connection_key(connection) == id;
                });
        };
        const Json::Value& main = models["main"];
        if (!main.isNull()) {
            if (!main.isObject()) {
                return fail(
                    Status::CONFIG_ERROR, "'models.main' must be an object");
            }
            const std::string provider = string_or_empty(main, "provider");
            const std::string model    = string_or_empty(main, "model");
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
            const std::string reasoning
                = string_or_empty(main, "reasoning_effort");
            out.reasoning_effort = reasoning.empty()
                ? std::optional<std::string> { }
                : std::optional<std::string> { reasoning };
        }
        const auto parse_model = [&](const char* key, SubagentRole role) {
            const Json::Value& value = models[key];
            if (value.isNull())
                return true;
            if (!value.isObject())
                return false;
            SubagentModelConfig config;
            config.provider = string_or_empty(value, "provider");
            config.model    = string_or_empty(value, "model");
            config.variant  = string_or_empty(value, "reasoning_effort");
            if (config.provider.empty() != config.model.empty())
                return false;
            if (!config.provider.empty()
                && !connection_exists(config.provider)) {
                return false;
            }
            if (!config.variant.empty() && config.variant != "off"
                && config.variant != "low" && config.variant != "default"
                && config.variant != "medium" && config.variant != "high") {
                return false;
            }
            out.subagents[role] = std::move(config);
            return true;
        };
        if (!parse_model("builder", SubagentRole::BUILDER)
            || !parse_model("researcher", SubagentRole::RESEARCH)
            || !parse_model("basic", SubagentRole::BASIC)) {
            return fail(Status::CONFIG_ERROR, "invalid models configuration");
        }
    }

    const Json::Value& skills = root["skills"];
    if (!skills.isNull()) {
        if (!skills.isObject())
            return fail(Status::CONFIG_ERROR, "'skills' must be an object");
        auto parse_map = [&](const Json::Value& value, auto& target) -> bool {
            if (!value.isObject())
                return false;
            for (const auto& name : value.getMemberNames()) {
                SkillPolicy policy;
                if (!value[name].isString()
                    || !parse_skill_policy(value[name].asString(), policy))
                    return false;
                target[name] = policy;
            }
            return true;
        };
        if (skills.isMember("global")
            && !parse_map(skills["global"], out.global_skills))
            return fail(Status::CONFIG_ERROR, "invalid global skill policy");
        const Json::Value& projects = skills["projects"];
        if (!projects.isNull()) {
            if (!projects.isObject())
                return fail(Status::CONFIG_ERROR,
                    "'skills.projects' must be an object");
            for (const auto& project_path : projects.getMemberNames()) {
                if (!parse_map(projects[project_path],
                        out.project_skills[project_path]))
                    return fail(
                        Status::CONFIG_ERROR, "invalid project skill policy");
            }
        }
    }

    return Status::OK;
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
        Json::Value root(Json::objectValue);
        Json::Value providers(Json::arrayValue);
        for (const Connection& conn : cfg.providers) {
            Json::Value entry(Json::objectValue);
            entry["id"] = conn.id;
            if (!conn.endpoint.empty()) {
                entry["endpoint"] = conn.endpoint;
            }
            if (!conn.api_key.empty()) {
                entry["api_key"] = conn.api_key;
            }
            if (!conn.refresh_token.empty()) {
                entry["refresh_token"] = conn.refresh_token;
            }
            if (conn.expires_at != 0) {
                entry["expires_at"] = conn.expires_at;
            }
            if (!conn.account_id.empty()) {
                entry["account_id"] = conn.account_id;
            }
            if (!conn.label.empty()) {
                entry["label"] = conn.label;
            }
            Json::Value dialects(Json::objectValue);
            for (const auto& [model, standard] : conn.dialects) {
                dialects[model] = dialect_str(standard);
            }
            if (!dialects.empty()) {
                entry["dialects"] = std::move(dialects);
            }
            providers.append(entry);
        }
        root["providers"] = providers;

        Json::Value models(Json::objectValue);
        Json::Value main(Json::objectValue);
        if (cfg.last_used) {
            main["provider"] = cfg.last_used->provider;
            main["model"]    = cfg.last_used->model;
        }
        main["reasoning_effort"]  = cfg.reasoning_effort.value_or("off");
        models["main"]            = std::move(main);
        const auto write_subagent = [&](const char* key, SubagentRole role) {
            const auto found = cfg.subagents.find(role);
            Json::Value value(Json::objectValue);
            if (found != cfg.subagents.end()
                && !found->second.provider.empty()) {
                value["provider"] = found->second.provider;
                value["model"]    = found->second.model;
            }
            value["reasoning_effort"]
                = found == cfg.subagents.end() || found->second.variant.empty()
                ? to_config_effort(subagent_default_variant(role))
                : to_config_effort(found->second.variant);
            models[key] = std::move(value);
        };
        write_subagent("builder", SubagentRole::BUILDER);
        write_subagent("researcher", SubagentRole::RESEARCH);
        write_subagent("basic", SubagentRole::BASIC);
        root["models"] = std::move(models);

        Json::Value skills(Json::objectValue);
        Json::Value global(Json::objectValue);
        for (const auto& [name, policy] : cfg.global_skills)
            global[name] = skill_policy_str(policy);
        skills["global"] = global;
        Json::Value projects(Json::objectValue);
        for (const auto& [project_path, policies] : cfg.project_skills) {
            Json::Value entry(Json::objectValue);
            for (const auto& [name, policy] : policies)
                entry[name] = skill_policy_str(policy);
            projects[project_path] = entry;
        }
        skills["projects"] = projects;
        root["skills"]     = skills;

        return write_json_file(path, root, "  ");
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
