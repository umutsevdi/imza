#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
namespace ursa {
struct DiffRow {
    enum class Kind { SAME, REMOVE, ADD };
    Kind kind = Kind::SAME;
    std::optional<std::size_t> left_no;
    std::optional<std::size_t> right_no;
    std::string left;
    std::string right;
};

struct DiffView {
    std::string file;
    std::vector<DiffRow> rows;
};

struct ShellExit {
    int code;
};

struct ShellTimeout {
    std::chrono::seconds duration;
};

using ShellStatus = std::variant<ShellExit, ShellTimeout>;

enum class Status {
    OK,
    NETWORK_ERROR,
    INVALID_URL,
    JSON_ERROR,
    API_ERROR,
    RATE_LIMITED,
    BUDGET_EXCEEDED,
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
    enum class ApprovalReason {
        TOOL_PERMISSION,
        OUTSIDE_WORKSPACE
    };

    std::string name;
    std::string args;
    std::string description { };
    std::string id { };
    ApprovalReason approval_reason = ApprovalReason::TOOL_PERMISSION;
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
    std::string prompt { };
};

struct ModalAnswer {
    std::vector<QuestionAnswer> cards;
};

struct ConnectResult {
    std::string provider_id;
    std::string endpoint;
    std::string api_key;
    bool persist = true;
};

struct ModelChoice {
    std::string connection_id;
    std::string model_id;
};

struct VariantChoice {
    std::string effort;
};

using ModalResult = std::variant<std::monostate, ToolVerdict, ModalAnswer,
    ConnectResult, ModelChoice, VariantChoice>;

struct ModelPricing {
    double input_per_1k  = 0.0;
    double output_per_1k = 0.0;
    double cache_read_per_1k = 0.0;
    double cache_write_per_1k = 0.0;
    std::uint64_t context_limit = 0;
};

struct Connection {
    std::string id;
    std::string provider_id;
    std::string endpoint;
    std::string api_key;
    std::map<std::string, ApiStandard> dialects;
};

struct LastUsed {
    std::string provider;
    std::string model;
};

struct Config {
    std::vector<Connection> providers;
    std::optional<LastUsed> last_used;
    std::optional<std::string> reasoning_effort;
};

Status load_config(const std::filesystem::path& path, Config& out,
    std::string* error = nullptr);
Status save_config(const std::filesystem::path& path, const Config& cfg);
std::filesystem::path base_config_dir(void);
std::filesystem::path config_path(void);
std::filesystem::path presets_path(void);

} // namespace ursa
