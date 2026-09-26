#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace imza {

struct ApplicationComponent { };

struct Attachment {
    enum class Type { TEXT, IMAGE, PDF };

    std::string path;
    std::string content;
    Type type = Type::TEXT;
    std::string media_type;

    const char* type_name() const
    {
        switch (type) {
        case Type::TEXT: return "text";
        case Type::IMAGE: return "image";
        case Type::PDF: return "pdf";
        }
        return "text";
    }

    static std::optional<Type> parse_type(std::string_view name)
    {
        if (name == "text") {
            return Type::TEXT;
        }
        if (name == "image") {
            return Type::IMAGE;
        }
        if (name == "pdf") {
            return Type::PDF;
        }
        return std::nullopt;
    }
};

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
inline constexpr std::array<std::string_view, 5> REASONING_EFFORTS { "off",
    "low", "default", "medium", "high" };

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
    SERVER_ERROR,
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
    case Status::SERVER_ERROR: return "Provider server error.";
    case Status::CANCELLED: return "Cancelled.";
    case Status::TIMEOUT: return "Timed out.";
    case Status::CONFIG_ERROR: return "Configuration error.";
    }
    return "Unknown error.";
}

enum class ApiStandard { OPENAI, OPENAI_RESPONSES, ANTHROPIC };

// Session interaction mode: PLAN reads and analyzes, BUILD may mutate.
enum class SessionMode { PLAN, BUILD };

enum class SkillPolicy { ALLOW, ASK, DENY };

} // namespace imza
