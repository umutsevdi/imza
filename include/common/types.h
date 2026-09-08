#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace imza {

struct ApplicationComponent { };

struct SavedSession {
    std::filesystem::path path;
    std::string title;
    std::string saved_at;

    bool operator==(const SavedSession&) const = default;
};

enum RuntimeFlag : std::uint8_t {
    NONE             = 0,
    WEB              = 1U << 0,
    SHELL            = 1U << 1,
    ATTENDED         = 1U << 2,
    SKIP_PERMISSIONS = 1U << 3,
};

constexpr RuntimeFlag interactive_runtime_flags()
{
    return static_cast<RuntimeFlag>(WEB | SHELL | ATTENDED);
}

// Reasoning-effort alias: config displays/stores "default" where the wire
// API spells it "medium".
inline std::string to_config_effort(std::string_view effort)
{
    return effort == "medium" ? "default" : std::string(effort);
}

inline std::string to_wire_effort(std::string_view effort)
{
    return effort == "default" ? "medium" : std::string(effort);
}

enum class Status {
    OK,
    NETWORK_ERROR,
    INVALID_URL,
    JSON_ERROR,
    API_ERROR,
    RATE_LIMITED,
    BUDGET_EXCEEDED,
    CANCELLED,
    TIMEOUT,
    CONFIG_ERROR
};

inline std::string error_text(Status st)
{
    switch (st) {
    case Status::OK: return "";
    case Status::NETWORK_ERROR: return "Network error.";
    case Status::INVALID_URL: return "Invalid API URL.";
    case Status::JSON_ERROR: return "Malformed response from provider.";
    case Status::API_ERROR: return "API error.";
    case Status::RATE_LIMITED: return "Rate limited by provider.";
    case Status::BUDGET_EXCEEDED:
        return "Out of budget / insufficient credits.";
    case Status::CANCELLED: return "Cancelled.";
    case Status::TIMEOUT: return "Timed out.";
    case Status::CONFIG_ERROR: return "Configuration error.";
    }
    return "Unknown error.";
}

enum class ApiStandard { OPENAI, ANTHROPIC };

enum class SkillPolicy { ALLOW, ASK, DENY };

} // namespace imza
