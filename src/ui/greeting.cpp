#include "ui/greeting.h"

#include "common/util.h"

#include <array>
#include <cstdint>

namespace imza {

namespace {

    constexpr int NIGHT_START_HOUR = 22;
    constexpr int NIGHT_END_HOUR   = 5;

    constexpr std::array<std::string_view, 5> NIGHT_LINES = {
        "It's getting late — remember to take care of yourself.",
        "The code will still be here tomorrow; so should\na well-rested you.",
        "Consider calling it a night.",
        "Rest is part of the loop too.",
        "Ship when it's right, not when it's late.",
    };

    constexpr std::array<std::string_view, 13> DAY_LINES = {
        "Ready when you are.",
        "What's the plan?",
        "Let's build something worth reviewing.",
        "Give each subagent role its own model with /subagents.",
        "Subagents share your permission grants.",
        "Context compacts automatically near 80% of the model's\nlimit.",
        "Type /make-skill to draft a skill from a workflow\ndescription.",
        "Flag MCP servers to autoload so they connect at\nstartup.",
        "Jump straight into a saved session\nwith imza -s <session-id>.",
        "You can add the same provider twice and switch \nwhen one is rate "
        "limited.",
        "Keep up to date with imza --update.",
        "Name extra instruction files in your config to \nload house rules.",
        "You can pre-approve directories and commands in\nyour config.",
    };

    // FNV-1a over the seed bytes and bucket, so the pick is stable and
    // spreads across the pool as the date advances.
    std::size_t mix(unsigned seed, TimeBucket bucket)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto feed    = [&hash](std::uint8_t byte) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        };
        for (int shift = 0; shift < 32; shift += 8) {
            feed(static_cast<std::uint8_t>((seed >> shift) & 0xFF));
        }
        feed(static_cast<std::uint8_t>(bucket));
        return static_cast<std::size_t>(hash);
    }

} // namespace

TimeBucket time_bucket(int hour)
{
    return (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR)
        ? TimeBucket::NIGHT
        : TimeBucket::DAY;
}

std::span<const std::string_view> greeting_pool(TimeBucket bucket)
{
    return bucket == TimeBucket::NIGHT
        ? std::span<const std::string_view>(NIGHT_LINES)
        : std::span<const std::string_view>(DAY_LINES);
}

std::string greeting_line(TimeBucket bucket, unsigned seed)
{
    const auto pool = greeting_pool(bucket);
    return std::string(pool[mix(seed, bucket) % pool.size()]);
}

std::string greeting_line_for(std::time_t now)
{
    const std::tm local     = local_tm(now);
    const TimeBucket bucket = time_bucket(local.tm_hour);
    const unsigned seed     = static_cast<unsigned>(local.tm_yday)
        + static_cast<unsigned>(local.tm_year) * 366u;
    return greeting_line(bucket, seed);
}

} // namespace imza
