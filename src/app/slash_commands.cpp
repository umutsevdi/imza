#include "app/slash_commands.h"
#include "common/util.h"

namespace imza {

std::span<const SlashCommand> slash_commands()
{
    static constexpr SlashCommand commands[] = {
        { "/new", "save this session and start a new one",
            SlashCommand::Action::NEW },
        { "/cd", "change the session working directory",
            SlashCommand::Action::CHDIR, true },
        { "/auto", "turn permission auto-approval on or off",
            SlashCommand::Action::AUTO, true },
        { "/sessions", "load or delete saved sessions",
            SlashCommand::Action::SESSIONS },
        { "/connect", "manage provider connections",
            SlashCommand::Action::CONNECT },
        { "/model", "pick the active model", SlashCommand::Action::MODEL },
        { "/variant", "pick reasoning effort", SlashCommand::Action::VARIANT },
        { "/mcp", "manage MCP servers", SlashCommand::Action::MCP },
        { "/subagents", "configure subagent models",
            SlashCommand::Action::SUBAGENTS },
        { "/skills", "manage or browse skills", SlashCommand::Action::SKILLS },
        { "/compact", "summarize and compact the conversation",
            SlashCommand::Action::COMPACT },
        { "/make-skill", "draft a skill from a workflow description",
            SlashCommand::Action::MAKE_SKILL, true },
        { "/changelog", "show the version history",
            SlashCommand::Action::CHANGELOG },
        { "/exit", "quit imza", SlashCommand::Action::EXIT },
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
