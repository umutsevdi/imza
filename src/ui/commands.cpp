#include "commands.h"

#include <cctype>

namespace ursa {

namespace {

    std::string to_lower(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            out += static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    }

} // namespace

std::vector<SlashCommand> slash_commands(const Config&)
{
    return {
        { "/help", "show available commands", SlashCommand::Action::HELP },
        { "/exit", "quit ursa", SlashCommand::Action::EXIT },
        { "/settings", "open settings", SlashCommand::Action::SETTINGS },
        { "/demo", "run scripted modal demo", SlashCommand::Action::DEMO },
    };
}

const SlashCommand* find_command(
    const std::vector<SlashCommand>& commands, std::string_view name)
{
    const std::string key = to_lower(name);
    for (const auto& c : commands) {
        if (to_lower(c.name) == key) {
            return &c;
        }
    }
    return nullptr;
}

} // namespace ursa
