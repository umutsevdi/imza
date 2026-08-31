#pragma once

#include <string>
#include <string_view>

namespace ursa {

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

} // namespace ursa
