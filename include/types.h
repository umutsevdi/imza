#pragma once

#include <filesystem>
#include <string>
#include <variant>
#include <vector>
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

enum class ToolDecision { ACCEPT, ACCEPT_ALWAYS, REJECT };

struct ToolVerdict {
    ToolDecision decision = ToolDecision::REJECT;
    std::string reason;
};

struct ToolCallRequest {
    std::string name;
    std::string args;
    std::string description;
};

struct QuestionCard {
    std::string prompt;
    std::vector<std::string> options;
    bool multi     = false;
    bool free_text = false;
};

using QuestionForm = std::vector<QuestionCard>;

struct QuestionAnswer {
    std::vector<std::string> selected;
    std::string free_text;
};

struct ModalAnswer {
    std::vector<QuestionAnswer> cards;
};

using ModalResult = std::variant<std::monostate, ToolVerdict, ModalAnswer>;

struct Config {
    ApiStandard standard = ApiStandard::OPENAI;
    std::string api_base;
    std::string api_key;
    std::string model;
};

Status load_config(const std::filesystem::path& path, Config& out);
std::filesystem::path config_path(void);

} // namespace ursa
