#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace imza {

struct ShellInvocation {
    std::string program;
    std::optional<std::string> subcommand;
    std::vector<std::string> arguments;

    bool operator==(const ShellInvocation&) const = default;
};

struct ShellAnalysis {
    enum class Reuse { SESSION, ONCE };

    std::vector<ShellInvocation> invocations;
    Reuse reuse = Reuse::ONCE;
};

// Shell command analysis and the read-only auto-accept catalogs. The
// permission evaluator turns an analysis into a verdict; the lua shell
// binding uses it to enforce the one-command rule; the CLI parses
// startup grants with it. Execution itself lives in platform's
// command_runner, not here.
ShellAnalysis analyze_shell(std::string_view command);
bool shell_builtin_allowed(std::string_view program);
bool shell_readonly_allowed(const ShellInvocation& invocation);

} // namespace imza
