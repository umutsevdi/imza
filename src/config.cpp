#include "types.h"

#include <json/json.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ursa {

std::filesystem::path config_path()
{
#if defined(_WIN32)
    const char* appdata        = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? appdata : ".";
    return base / "ursa" / "config.json";
#elif defined(__APPLE__)
    const char* home           = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
    return base / "Library" / "Application Support" / "ursa" / "config.json";
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
    return base / "ursa" / "config.json";
#endif
}

Status load_config(const std::filesystem::path& path, Config& out,
    std::string* error)
{
    const auto fail = [&](Status st, const std::string& msg) {
        if (error != nullptr) {
            *error = msg;
        }
        return st;
    };

    std::ifstream file(path);
    if (!file) {
        return fail(Status::CONFIG_ERROR, "cannot open " + path.string());
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

    const auto string_field = [&](const char* key) -> std::string {
        const Json::Value& v = root[key];
        return v.isString() ? v.asString() : std::string();
    };

    out.api_base = string_field("api_base");
    out.api_key  = string_field("api_key");
    out.model    = string_field("model");

    const std::string standard = string_field("standard");
    out.standard = (standard == "anthropic") ? ApiStandard::ANTHROPIC
                                             : ApiStandard::OPENAI;

    if (out.api_key.empty() || out.model.empty()) {
        return fail(Status::CONFIG_ERROR,
            "config requires non-empty 'api_key' and 'model'");
    }
    return Status::OK;
}

} // namespace ursa
