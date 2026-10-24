#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace imza {

struct SlashCommand {
    enum class Action {
        EXIT,
        NEW,
        CHANGELOG,
        CONNECT,
        MODEL,
        VARIANT,
        SUBAGENTS,
        SESSIONS,
        SKILLS,
        MCP,
        COMPACT,
        MAKE_SKILL,
        AUTO,
        PERMISSIONS,
        CHDIR,
        UNDO
    };

    std::string_view name;
    std::string_view desc;
    Action action = Action::EXIT;
    // Argument-taking commands complete without submitting, so the user
    // can type the argument; the rest execute on the same Enter.
    bool takes_argument = false;
    // Other harnesses' names for this action, without the leading slash.
    // Aliases never shadow a canonical name; `find_command` resolves the
    // canonical name first.
    std::span<const std::string_view> aliases;
};

// One autocomplete hit: the command to run and the alias that matched it,
// empty when the canonical name did.
struct CommandMatch {
    const SlashCommand* command = nullptr;
    std::string_view alias;
};

std::span<const SlashCommand> slash_commands();
const SlashCommand* find_command(std::string_view name);

// Ranked suggestions for a partially typed command (with or without the
// leading slash): canonical-name prefixes first, then alias prefixes, then
// subsequences of either. Order is stable within a tier.
std::vector<CommandMatch> match_commands(std::string_view query);

// Closest command name or alias to a mistyped one, when it is close enough
// to be worth suggesting.
std::optional<CommandMatch> suggest_command(std::string_view query);

} // namespace imza
