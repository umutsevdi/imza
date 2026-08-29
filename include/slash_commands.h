#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "session.h"

namespace ursa {

struct ApplicationState;

struct SlashCommand {
    enum class Action { EXIT, NEW, SYSTEM_PROMPT, CONNECT, MODEL, VARIANT,
        SUBAGENTS, SESSIONS, SKILLS };

    std::string_view name;
    std::string_view desc;
    Action action = Action::EXIT;
};

struct SlashCommandContext {
    const ApplicationState& state;
    std::function<void()> exit;
    std::function<void()> new_session;
    std::function<void(ModalPayload)> present_modal;
    std::function<SessionsModal()> sessions_modal;
    std::function<SkillsModal()> skills_modal;
    std::function<std::string()> system_prompt;
    std::function<void(std::string)> set_error;
};

std::span<const SlashCommand> slash_commands();
const SlashCommand* find_command(std::string_view name);
void run_slash_command(const SlashCommandContext& context,
    std::string_view command);

} // namespace ursa
