#include "types.h"

#include <json/json.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ursa {

std::filesystem::path base_config_dir()
{
#if defined(_WIN32)
    const char* appdata        = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? appdata : ".";
    return base / "ursa";
#elif defined(__APPLE__)
    const char* home           = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
    return base / "Library" / "Application Support" / "ursa";
#else
    const char* xdg  = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else if (home) {
        base = home;
        base /= ".config";
    } else {
        base = ".config";
    }
    return base / "ursa";
#endif
}

std::filesystem::path config_path()
{
    return base_config_dir() / "config.json";
}

std::filesystem::path presets_path()
{
    return base_config_dir() / "presets.json";
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
        if (text == "anthropic") {
            out = ApiStandard::ANTHROPIC;
            return true;
        }
        return false;
    }

    const char* dialect_str(ApiStandard standard)
    {
        return standard == ApiStandard::ANTHROPIC ? "anthropic" : "openai";
    }

    bool parse_skill_policy(const std::string& text, SkillPolicy& out)
    {
        if (text == "allow") out = SkillPolicy::ALLOW;
        else if (text == "ask") out = SkillPolicy::ASK;
        else if (text == "deny") out = SkillPolicy::DENY;
        else return false;
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

Status load_config(const std::filesystem::path& path, Config& out,
    std::string* error)
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
            conn.id          = string_or_empty(entry, "id");
            conn.provider_id = string_or_empty(entry, "provider_id");
            conn.endpoint    = string_or_empty(entry, "endpoint");
            conn.api_key     = string_or_empty(entry, "api_key");
            if (conn.id.empty() || conn.provider_id.empty()) {
                return fail(Status::CONFIG_ERROR,
                    "provider entry requires non-empty 'id' and 'provider_id'");
            }
            const Json::Value& dialects = entry["dialects"];
            if (!dialects.isNull()) {
                if (!dialects.isObject()) {
                    return fail(Status::CONFIG_ERROR,
                        "'dialects' must be an object");
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
    }

    const Json::Value& last = root["last_used"];
    if (!last.isNull()) {
        if (!last.isObject()) {
            return fail(Status::CONFIG_ERROR, "'last_used' must be an object");
        }
        LastUsed used;
        used.provider = string_or_empty(last, "provider");
        used.model    = string_or_empty(last, "model");
        bool found    = false;
        for (const Connection& conn : out.providers) {
            if (conn.id == used.provider) {
                found = true;
                break;
            }
        }
        if (!found) {
            return fail(Status::CONFIG_ERROR,
                "'last_used.provider' does not resolve to a connection");
        }
        out.last_used = std::move(used);
    }

    const Json::Value& effort = root["reasoning_effort"];
    if (effort.isString() && !effort.asString().empty()) {
        out.reasoning_effort = effort.asString();
    }

    const Json::Value& skills = root["skills"];
    if (!skills.isNull()) {
        if (!skills.isObject()) return fail(Status::CONFIG_ERROR, "'skills' must be an object");
        auto parse_map = [&](const Json::Value& value, auto& target) -> bool {
            if (!value.isObject()) return false;
            for (const auto& name : value.getMemberNames()) {
                SkillPolicy policy;
                if (!value[name].isString() || !parse_skill_policy(value[name].asString(), policy)) return false;
                target[name] = policy;
            }
            return true;
        };
        if (skills.isMember("global") && !parse_map(skills["global"], out.global_skills))
            return fail(Status::CONFIG_ERROR, "invalid global skill policy");
        const Json::Value& projects = skills["projects"];
        if (!projects.isNull()) {
            if (!projects.isObject()) return fail(Status::CONFIG_ERROR, "'skills.projects' must be an object");
            for (const auto& project_path : projects.getMemberNames()) {
                if (!parse_map(projects[project_path],
                        out.project_skills[project_path]))
                    return fail(Status::CONFIG_ERROR, "invalid project skill policy");
            }
        }
    }

    return Status::OK;
}

Status save_config(const std::filesystem::path& path, const Config& cfg)
{
    Json::Value root(Json::objectValue);

    Json::Value providers(Json::arrayValue);
    for (const Connection& conn : cfg.providers) {
        Json::Value entry(Json::objectValue);
        entry["id"]          = conn.id;
        entry["provider_id"] = conn.provider_id;
        if (!conn.endpoint.empty()) {
            entry["endpoint"] = conn.endpoint;
        }
        entry["api_key"] = conn.api_key;
        if (!conn.dialects.empty()) {
            Json::Value dialects(Json::objectValue);
            for (const auto& [model, standard] : conn.dialects) {
                dialects[model] = dialect_str(standard);
            }
            entry["dialects"] = dialects;
        }
        providers.append(entry);
    }
    root["providers"] = providers;

    if (cfg.last_used) {
        Json::Value last(Json::objectValue);
        last["provider"] = cfg.last_used->provider;
        last["model"]    = cfg.last_used->model;
        root["last_used"] = last;
    }

    if (cfg.reasoning_effort && !cfg.reasoning_effort->empty()) {
        root["reasoning_effort"] = *cfg.reasoning_effort;
    }


    Json::Value skills(Json::objectValue);
    Json::Value global(Json::objectValue);
    for (const auto& [name, policy] : cfg.global_skills) global[name] = skill_policy_str(policy);
    skills["global"] = global;
    Json::Value projects(Json::objectValue);
    for (const auto& [project_path, policies] : cfg.project_skills) {
        Json::Value entry(Json::objectValue);
        for (const auto& [name, policy] : policies) entry[name] = skill_policy_str(policy);
        projects[project_path] = entry;
    }
    skills["projects"] = projects;
    root["skills"] = skills;

    const std::filesystem::path parent = path.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file) {
            return Status::CONFIG_ERROR;
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        file << Json::writeString(builder, root) << "\n";
        if (!file) {
            return Status::CONFIG_ERROR;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return Status::CONFIG_ERROR;
    }
    return Status::OK;
}

} // namespace ursa
