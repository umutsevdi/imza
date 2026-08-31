#include "slash_commands.h"

#include "application_state.h"
#include "provider_store.h"
#include "util.h"

#include <string>

namespace ursa {

std::span<const SlashCommand> slash_commands()
{
    static constexpr SlashCommand commands[] = {
        { "/new", "save this session and start a new one",
            SlashCommand::Action::NEW },
        { "/exit", "quit ursa", SlashCommand::Action::EXIT },
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
        if (to_lower(command.name) == key)
            return &command;
    }
    return nullptr;
}

void run_slash_command(
    const SlashCommandContext& context, std::string_view command)
{
    const SlashCommand* found = find_command(command);
    if (found == nullptr) {
        context.set_error("Unknown command: " + std::string(command) + ".");
        return;
    }

    switch (found->action) {
    case SlashCommand::Action::EXIT: context.exit(); break;
    case SlashCommand::Action::NEW: context.new_session(); break;
    case SlashCommand::Action::CONNECT:
        context.present_modal(ConnectModal { ConnectModal::Entry::MANAGE });
        break;
    case SlashCommand::Action::MODEL:
        if (context.state.providers->connections().empty()) {
            context.set_error("No connections — run /connect first.");
            break;
        }
        context.present_modal(ConnectModal { ConnectModal::Entry::PICK_MODEL });
        break;
    case SlashCommand::Action::VARIANT: {
        std::string current = to_config_effort(
            context.state.providers->status().reasoning_effort);
        context.present_modal(VariantModal {
            { "off", "low", "default", "high" }, std::move(current) });
        break;
    }
    case SlashCommand::Action::SUBAGENTS:
        context.present_modal(ConnectModal { ConnectModal::Entry::SUBAGENTS });
        break;
    case SlashCommand::Action::SESSIONS:
        context.present_modal(context.sessions_modal());
        break;
    case SlashCommand::Action::SKILLS:
        context.present_modal(context.skills_modal());
        break;
    case SlashCommand::Action::SYSTEM_PROMPT:
        context.present_modal(ViewerModal {
            "System prompt", context.system_prompt(), "text", 1, false, "" });
        break;
    }
}

} // namespace ursa
