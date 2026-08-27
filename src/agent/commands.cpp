#include "commands.h"

#include "controller.h"
#include "util.h"

#include <string>
#include <string_view>

namespace ursa {

std::vector<SlashCommand> slash_commands()
{
    return {
        { "/help", "show available commands", SlashCommand::Action::HELP },
        { "/exit", "quit ursa", SlashCommand::Action::EXIT },
        { "/connect", "manage provider connections",
            SlashCommand::Action::CONNECT },
        { "/model", "pick the active model", SlashCommand::Action::MODEL },
        { "/variant", "pick reasoning effort", SlashCommand::Action::VARIANT },
        { "/prompt", "show the generated system prompt",
            SlashCommand::Action::SYSTEM_PROMPT },
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

void Controller::run_slash(std::string_view cmd)
{
    const SlashCommand* found = find_command(commands_, cmd);
    if (found == nullptr) {
        set_error("unknown command: " + std::string(cmd));
        return;
    }
    switch (found->action) {
    case SlashCommand::Action::EXIT: on_exit_(); break;
    case SlashCommand::Action::HELP: enqueue_user_modal(HelpModal { }); break;
    case SlashCommand::Action::CONNECT:
        enqueue_user_modal(ConnectModal { ConnectModal::Entry::MANAGE });
        break;
    case SlashCommand::Action::MODEL: {
        bool any = false;
        {
            std::lock_guard lock(data_mutex_);
            any = !cfg_.providers.empty();
        }
        if (!any) {
            set_error("no connections — run /connect first");
            break;
        }
        enqueue_user_modal(ConnectModal { ConnectModal::Entry::PICK_MODEL });
        break;
    }
    case SlashCommand::Action::VARIANT: {
        std::string current;
        {
            std::lock_guard lock(data_mutex_);
            current = cfg_.reasoning_effort.value_or("default");
            if (current == "medium") {
                current = "default";
            }
        }
        enqueue_user_modal(VariantModal {
            { "off", "low", "default", "high" }, current });
        break;
    }
    case SlashCommand::Action::SYSTEM_PROMPT:
        enqueue_user_modal(ViewerModal {
            "System prompt", _system_prompt(), "text", 1, false });
        break;
    }
}

} // namespace ursa