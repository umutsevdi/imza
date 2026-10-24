#include "app/slash_commands.h"
#include "common/util.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace imza {

namespace {

    constexpr std::array<std::string_view, 1> EXIT_ALIASES { "quit" };
    constexpr std::array<std::string_view, 2> NEW_ALIASES { "clear", "reset" };
    constexpr std::array<std::string_view, 1> COMPACT_ALIASES { "compress" };
    constexpr std::array<std::string_view, 3> VARIANT_ALIASES { "reasoning",
        "effort", "reasoning-effort" };
    constexpr std::array<std::string_view, 1> MODEL_ALIASES { "models" };
    constexpr std::array<std::string_view, 1> SESSIONS_ALIASES { "resume" };
    constexpr std::array<std::string_view, 1> SUBAGENTS_ALIASES { "agents" };
    constexpr std::array<std::string_view, 2> CONNECT_ALIASES { "login",
        "auth" };
    constexpr std::array<std::string_view, 2> CHDIR_ALIASES { "dir",
        "directory" };
    constexpr std::array<std::string_view, 1> AUTO_ALIASES { "approvals" };
    constexpr std::array<std::string_view, 1> UNDO_ALIASES { "revert" };

    // The query without its leading slash, lowercased.
    std::string command_key(std::string_view query)
    {
        if (!query.empty() && query.front() == '/') {
            query.remove_prefix(1);
        }
        return to_lower(query);
    }

    bool is_subsequence(std::string_view needle, std::string_view haystack)
    {
        std::size_t at = 0;
        for (const char c : haystack) {
            if (at < needle.size() && needle[at] == c) {
                ++at;
            }
        }
        return at == needle.size();
    }

    // Levenshtein distance, bounded by the shorter string's length.
    std::size_t edit_distance(std::string_view a, std::string_view b)
    {
        std::vector<std::size_t> previous(b.size() + 1);
        std::vector<std::size_t> current(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) {
            previous[j] = j;
        }
        for (std::size_t i = 1; i <= a.size(); ++i) {
            current[0] = i;
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const std::size_t substitution
                    = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
                current[j] = std::min(
                    { previous[j] + 1, current[j - 1] + 1, substitution });
            }
            previous.swap(current);
        }
        return previous[b.size()];
    }

} // namespace

std::span<const SlashCommand> slash_commands()
{
    static constexpr SlashCommand commands[] = {
        { "/new", "save this session and start a new one",
            SlashCommand::Action::NEW, false, NEW_ALIASES },
        { "/cd", "change the session working directory",
            SlashCommand::Action::CHDIR, true, CHDIR_ALIASES },
        { "/auto", "turn permission auto-approval on or off",
            SlashCommand::Action::AUTO, true, AUTO_ALIASES },
        { "/permissions", "manage persistent permission grants",
            SlashCommand::Action::PERMISSIONS },
        { "/sessions", "load or delete saved sessions",
            SlashCommand::Action::SESSIONS, false, SESSIONS_ALIASES },
        { "/connect", "manage provider connections",
            SlashCommand::Action::CONNECT, false, CONNECT_ALIASES },
        { "/model", "pick the active model", SlashCommand::Action::MODEL, false,
            MODEL_ALIASES },
        { "/variant", "pick reasoning effort", SlashCommand::Action::VARIANT,
            false, VARIANT_ALIASES },
        { "/mcp", "manage MCP servers", SlashCommand::Action::MCP },
        { "/subagents", "configure subagent models",
            SlashCommand::Action::SUBAGENTS, false, SUBAGENTS_ALIASES },
        { "/skills", "manage or browse skills", SlashCommand::Action::SKILLS },
        { "/compact", "summarize and compact the conversation",
            SlashCommand::Action::COMPACT, false, COMPACT_ALIASES },
        { "/make-skill", "draft a skill from a workflow description",
            SlashCommand::Action::MAKE_SKILL, true },
        { "/undo", "revert the last file change the agent made",
            SlashCommand::Action::UNDO, false, UNDO_ALIASES },
        { "/changelog", "show the version history",
            SlashCommand::Action::CHANGELOG },
        { "/exit", "quit imza", SlashCommand::Action::EXIT, false,
            EXIT_ALIASES },
    };
    return commands;
}

const SlashCommand* find_command(std::string_view name)
{
    const std::string key = command_key(name);
    for (const SlashCommand& command : slash_commands()) {
        if (command_key(command.name) == key) {
            return &command;
        }
    }
    for (const SlashCommand& command : slash_commands()) {
        for (const std::string_view alias : command.aliases) {
            if (alias == key) {
                return &command;
            }
        }
    }
    return nullptr;
}

std::vector<CommandMatch> match_commands(std::string_view query)
{
    const std::string key = command_key(query);
    std::vector<CommandMatch> matches;
    // Three passes so the tiers stay ordered: canonical prefixes, alias
    // prefixes, then subsequences of either.
    for (const SlashCommand& command : slash_commands()) {
        if (command_key(command.name).starts_with(key)) {
            matches.push_back({ &command, { } });
        }
    }
    for (const SlashCommand& command : slash_commands()) {
        if (command_key(command.name).starts_with(key)) {
            continue;
        }
        for (const std::string_view alias : command.aliases) {
            if (alias.starts_with(key)) {
                matches.push_back({ &command, alias });
                break;
            }
        }
    }
    // A one- or two-letter query makes the subsequence tier noise; it
    // only earns its place once the user has typed enough to mean it.
    if (key.size() < 3) {
        return matches;
    }
    for (const SlashCommand& command : slash_commands()) {
        if (command_key(command.name).starts_with(key)) {
            continue;
        }
        const bool alias_prefix = std::ranges::any_of(command.aliases,
            [&](std::string_view alias) { return alias.starts_with(key); });
        if (alias_prefix) {
            continue;
        }
        if (is_subsequence(key, command_key(command.name))) {
            matches.push_back({ &command, { } });
            continue;
        }
        for (const std::string_view alias : command.aliases) {
            if (is_subsequence(key, alias)) {
                matches.push_back({ &command, alias });
                break;
            }
        }
    }
    return matches;
}

std::optional<CommandMatch> suggest_command(std::string_view query)
{
    const std::string key = command_key(query);
    if (key.empty()) {
        return std::nullopt;
    }
    // A typo is at most a couple of edits away; anything further is
    // somebody typing a different word, not a misspelling.
    const std::size_t limit = std::max<std::size_t>(1, key.size() / 3 + 1);
    std::optional<CommandMatch> best;
    std::size_t best_distance = limit + 1;
    const auto consider
        = [&](const SlashCommand& command, std::string_view candidate,
              std::string_view alias) {
              const std::size_t distance = edit_distance(key, candidate);
              if (distance < best_distance) {
                  best_distance = distance;
                  best          = CommandMatch { &command, alias };
              }
          };
    for (const SlashCommand& command : slash_commands()) {
        consider(command, command_key(command.name), { });
        for (const std::string_view alias : command.aliases) {
            consider(command, alias, alias);
        }
    }
    if (!best || best_distance > limit) {
        return std::nullopt;
    }
    return best;
}

} // namespace imza
