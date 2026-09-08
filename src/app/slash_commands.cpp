#include "app/slash_commands.h"
#include "common/util.h"

namespace imza {

std::span<const SlashCommand> slash_commands()
{
    static constexpr SlashCommand commands[] = {
        { "/new", "save this session and start a new one",
            SlashCommand::Action::NEW },
        { "/exit", "quit imza", SlashCommand::Action::EXIT },
        { "/connect", "manage provider connections",
            SlashCommand::Action::CONNECT },
        { "/model", "pick the active model", SlashCommand::Action::MODEL },
        { "/variant", "pick reasoning effort", SlashCommand::Action::VARIANT },
        { "/subagents", "configure subagent models",
            SlashCommand::Action::SUBAGENTS },
        { "/sessions", "load or delete saved sessions",
            SlashCommand::Action::SESSIONS },
        { "/skills", "manage discovered skills", SlashCommand::Action::SKILLS },
        { "/prompt", "show the generated system prompt",
            SlashCommand::Action::SYSTEM_PROMPT },
    };
    return commands;
}

const SlashCommand* find_command(std::string_view name)
{
    const std::string key = to_lower(name);
    for (const SlashCommand& command : slash_commands()) {
        if (to_lower(command.name) == key) {
            return &command;
        }
    }
    return nullptr;
}

} // namespace imza
