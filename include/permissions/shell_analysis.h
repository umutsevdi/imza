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
    // True when the program carries code the analyzer cannot read: an
    // interpreter, a script file, or a launcher with no nested command
    // string. Such an invocation is approval-once and never grantable.
    bool unresolvable = false;

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

// Programs that delete filesystem content. They never run through the
// shell: deletion goes through imza.fs.remove so it cannot bypass the
// filesystem permission gate. Absolute: no session, startup, or
// --allow-cmd grant re-enables one.
bool shell_deletion_denied(std::string_view program);

// Programs that run code the analyzer cannot read: shells, command
// multiplexers, wrappers, and interpreters. analyze_shell unwraps what it
// can, so these names never survive into a classifiable invocation; a
// grant naming one is refused, because it would authorize every command
// the program can run.
bool shell_launcher_denied(std::string_view program);

} // namespace imza
