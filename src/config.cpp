#include "types.hpp"

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

Status load_config(const std::filesystem::path& path, Config& out)
{
    std::ifstream file(path);
    if (!file) {
        return Status::CONFIG_ERROR;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string err;
    std::istringstream parse_stream { text };
    if (!Json::parseFromStream(reader, parse_stream, &root, &err)) {
        return Status::CONFIG_ERROR;
    }

    out.api_base = root.get("api_base", "").asString();
    out.api_key  = root.get("api_key", "").asString();
    out.model    = root.get("model", "").asString();

    const std::string standard = root.get("standard", "openai").asString();
    out.standard
        = (standard == "anthropic") ? Standard::ANTHROPIC : Standard::OPENAI;

    if (out.api_key.empty() || out.model.empty()) {
        return Status::CONFIG_ERROR;
    }
    return Status::OK;
}

} // namespace ursa
