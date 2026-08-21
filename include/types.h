#pragma once

#include <filesystem>
#include <string>
namespace ursa {
enum class Status {
    OK,
    NETWORK_ERROR,
    INVALID_URL,
    JSON_ERROR,
    API_ERROR,
    UNSUPPORTED,
    CONFIG_ERROR
};

enum class ApiStandard { OPENAI, ANTHROPIC };

struct Config {
    ApiStandard standard = ApiStandard::OPENAI;
    std::string api_base;
    std::string api_key;
    std::string model;
};

Status load_config(const std::filesystem::path& path, Config& out);
std::filesystem::path config_path(void);

} // namespace ursa
