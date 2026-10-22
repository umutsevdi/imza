#include "permissions/shell_analysis.h"

#include "common/util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>

namespace imza {

namespace {

    enum class Quote { NONE, SINGLE, DOUBLE };

    bool is_assignment(std::string_view word)
    {
        const std::size_t equals = word.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            return false;
        }
        if (!(std::isalpha(static_cast<unsigned char>(word.front()))
                || word.front() == '_')) {
            return false;
        }
        for (std::size_t index = 1; index < equals; ++index) {
            const unsigned char character
                = static_cast<unsigned char>(word[index]);
            if (!std::isalnum(character) && character != '_') {
                return false;
            }
        }
        return true;
    }

    bool is_flag(const std::string& word)
    {
        return !word.empty() && word.front() == '-';
    }

    // TRANSPARENT prefixes name the program they invoke (env, sudo);
    // SHELL programs take a command string in a flag (sh -c, cmd /c);
    // OPAQUE programs carry code the analyzer cannot read.
    enum class Launcher { NONE, TRANSPARENT, SHELL, OPAQUE };

    // Bound on launcher unwrapping depth; a deeper chain stops and the
    // remaining program is unresolvable.
    constexpr int MAX_LAUNCHER_DEPTH = 4;

    Launcher launcher_kind(std::string_view program)
    {
        static constexpr std::array<std::string_view, 10> transparent { "env",
            "sudo", "doas", "xargs", "nice", "nohup", "time", "stdbuf",
            "command", "exec" };
        static constexpr std::array<std::string_view, 10> shells { "sh", "bash",
            "zsh", "dash", "ksh", "fish", "cmd", "cmd.exe", "powershell",
            "pwsh" };
        static constexpr std::array<std::string_view, 9> opaque { "python",
            "python3", "perl", "ruby", "node", "php", "lua", "awk", "gawk" };
        for (const std::string_view candidate : transparent) {
            if (program == candidate) {
                return Launcher::TRANSPARENT;
            }
        }
        for (const std::string_view candidate : shells) {
            if (program == candidate) {
                return Launcher::SHELL;
            }
        }
        for (const std::string_view candidate : opaque) {
            if (program == candidate) {
                return Launcher::OPAQUE;
            }
        }
        return Launcher::NONE;
    }

    std::optional<std::string> shell_code_argument(
        std::string_view program, const std::vector<std::string>& words)
    {
        const bool cmd_style = program == "cmd" || program == "cmd.exe";
        for (std::size_t index = 1; index < words.size(); ++index) {
            const std::string& word = words[index];
            bool carries_code       = false;
            if (cmd_style) {
                carries_code = word == "/c" || word == "/C";
            } else if (word == "-c" || word == "-Command"
                || word == "--command") {
                carries_code = true;
            } else if (word.size() > 2 && word.front() == '-'
                && word[1] != '-') {
                carries_code = word.find('c') != std::string::npos;
            }
            if (carries_code && index + 1 < words.size()) {
                return words[index + 1];
            }
        }
        return std::nullopt;
    }

    ShellAnalysis analyze_shell_impl(std::string_view command, int depth);

    void append_invocation(
        ShellAnalysis& analysis, std::vector<std::string>& words, int depth)
    {
        std::size_t program = 0;
        while (program < words.size() && is_assignment(words[program])) {
            analysis.reuse = ShellAnalysis::Reuse::ONCE;
            ++program;
        }
        if (program == words.size()) {
            words.clear();
            return;
        }
        std::string name = words[program];
#ifdef _WIN32
        name = to_lower(name);
#endif
        const Launcher kind = launcher_kind(name);

        // The prefix's own assignments and flags are consumed here rather
        // than by the assignment rule below, which would otherwise mark
        // `env FOO=x ls` dynamic.
        if (kind == Launcher::TRANSPARENT && depth < MAX_LAUNCHER_DEPTH) {
            std::size_t next = program + 1;
            while (next < words.size()) {
                if (is_assignment(words[next])) {
                    ++next;
                    continue;
                }
                if (!is_flag(words[next])) {
                    break;
                }
                if (words[next].size() == 2 && words[next][1] != '-'
                    && next + 1 < words.size() && !is_flag(words[next + 1])) {
                    next += 2;
                    continue;
                }
                ++next;
            }
            if (next >= words.size()) {
                words.clear();
                ShellInvocation opaque;
                opaque.program      = std::move(name);
                opaque.unresolvable = true;
                analysis.invocations.push_back(std::move(opaque));
                return;
            }
            words.erase(words.begin(),
                words.begin() + static_cast<std::ptrdiff_t>(next));
            append_invocation(analysis, words, depth + 1);
            return;
        }

        // Readable code is replaced by the commands it names: `bash -c
        // 'ls | grep x'` becomes two invocations, which the one-command
        // rule rejects; `bash -c 'rm -rf x'` becomes a deletion.
        if (kind == Launcher::SHELL && depth < MAX_LAUNCHER_DEPTH) {
            if (const auto code = shell_code_argument(name, words)) {
                words.clear();
                const ShellAnalysis nested
                    = analyze_shell_impl(*code, depth + 1);
                if (nested.reuse == ShellAnalysis::Reuse::ONCE) {
                    analysis.reuse = ShellAnalysis::Reuse::ONCE;
                }
                for (const ShellInvocation& invocation : nested.invocations) {
                    analysis.invocations.push_back(invocation);
                }
                if (nested.invocations.empty()) {
                    ShellInvocation opaque;
                    opaque.program      = std::move(name);
                    opaque.unresolvable = true;
                    analysis.invocations.push_back(std::move(opaque));
                }
                return;
            }
        }

        ShellInvocation invocation;
        invocation.program = std::move(name);
        // A launcher whose code could not be unwrapped is approval-once
        // and never grantable: there is nothing to classify.
        invocation.unresolvable = kind != Launcher::NONE;
        // Subcommand identity: first word after the program that is not an
        // option. Options before the subcommand carry no grant identity; a
        // short option and its separate value (git -C build) are skipped
        // whole. Past the subcommand only flags are retained as arguments:
        // operands cannot widen a grant, and their values are consumed with
        // their option.
        std::size_t index = program + 1;
        while (index < words.size() && is_flag(words[index])) {
            if (words[index].size() == 2 && words[index][1] != '-'
                && index + 1 < words.size() && !is_flag(words[index + 1])) {
                ++index;
            }
            ++index;
        }
        if (index < words.size()) {
            invocation.subcommand = std::move(words[index]);
            ++index;
        }
        for (; index < words.size(); ++index) {
            if (!is_flag(words[index])) {
                continue;
            }
            if (words[index].size() == 2 && words[index][1] != '-'
                && index + 1 < words.size() && !is_flag(words[index + 1])) {
                ++index;
            }
            invocation.arguments.push_back(std::move(words[index]));
        }
        analysis.invocations.push_back(std::move(invocation));
        words.clear();
    }

    struct ReadOnlyCommand {
        std::string_view program;
        std::optional<std::string_view> subcommand;
        std::string_view flags;
    };

    bool flag_allowed(std::string_view flags, std::string_view argument)
    {
        std::size_t begin = 0;
        while (begin < flags.size()) {
            const std::size_t end = flags.find(' ', begin);
            const std::size_t last
                = end == std::string_view::npos ? flags.size() : end;
            if (flags.substr(begin, last - begin) == argument) {
                return true;
            }
            begin = last + 1;
        }
        return false;
    }

    bool is_separator(char character)
    {
        return character == ';' || character == '|' || character == '&'
            || character == '\n';
    }

    ShellAnalysis analyze_shell_impl(std::string_view command, int depth)
    {
        ShellAnalysis analysis;
        analysis.reuse = ShellAnalysis::Reuse::SESSION;
        std::vector<std::string> words;
        std::string word;
        Quote quote           = Quote::NONE;
        bool escaped          = false;
        bool word_started     = false;
        bool redirect_operand = false;

        const auto finish_word = [&] {
            if (word_started) {
                if (!redirect_operand) {
                    words.push_back(std::move(word));
                }
                word.clear();
                word_started     = false;
                redirect_operand = false;
            }
        };

        for (std::size_t index = 0; index < command.size(); ++index) {
            const char character = command[index];
            if (character == '>') {
                analysis.reuse = ShellAnalysis::Reuse::ONCE;
            }
            if (escaped) {
                word.push_back(character);
                word_started = true;
                escaped      = false;
                continue;
            }
            if (quote == Quote::SINGLE) {
                if (character == '\'') {
                    quote = Quote::NONE;
                } else {
                    word.push_back(character);
                }
                word_started = true;
                continue;
            }
            if (quote == Quote::DOUBLE) {
                if (character == '"') {
                    quote = Quote::NONE;
                } else if (character == '\\') {
                    // POSIX: inside double quotes a backslash is special
                    // only before $ ` " \ or newline; anywhere else both
                    // characters stay literal, so Windows paths survive.
                    const char next
                        = index + 1 < command.size() ? command[index + 1] : 0;
                    if (next == '$' || next == '`' || next == '"'
                        || next == '\\' || next == '\n') {
                        escaped = true;
                    } else {
                        word.push_back(character);
                    }
                } else {
                    if (character == '$' || character == '`') {
                        analysis.reuse = ShellAnalysis::Reuse::ONCE;
                    }
                    word.push_back(character);
                }
                word_started = true;
                continue;
            }
            if (character == '\\') {
                escaped      = true;
                word_started = true;
                continue;
            }
            if (character == '\'' || character == '"') {
                quote = character == '\'' ? Quote::SINGLE : Quote::DOUBLE;
                word_started = true;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(character))
                && character != '\n') {
                finish_word();
                continue;
            }
            if (character == '#' && !word_started) {
                while (index < command.size() && command[index] != '\n') {
                    ++index;
                }
                finish_word();
                append_invocation(analysis, words, depth);
                continue;
            }
            if (character == '>') {
                finish_word();
                redirect_operand = true;
                continue;
            }
            if (character == '<') {
                finish_word();
                if (index + 1 < command.size() && command[index + 1] == '<') {
                    analysis.reuse = ShellAnalysis::Reuse::ONCE;
                }
                redirect_operand = true;
                continue;
            }
            if (is_separator(character)) {
                finish_word();
                append_invocation(analysis, words, depth);
                while (index + 1 < command.size()
                    && is_separator(command[index + 1])
                    && command[index + 1] != '\n') {
                    ++index;
                }
                continue;
            }
            if (character == '$' || character == '`' || character == '('
                || character == ')' || character == '{' || character == '}') {
                analysis.reuse = ShellAnalysis::Reuse::ONCE;
            }
            word.push_back(character);
            word_started = true;
        }
        if (escaped || quote != Quote::NONE) {
            analysis.reuse = ShellAnalysis::Reuse::ONCE;
        }
        finish_word();
        append_invocation(analysis, words, depth);
        if (analysis.invocations.empty()) {
            analysis.reuse = ShellAnalysis::Reuse::ONCE;
        }
        return analysis;
    }

} // namespace

ShellAnalysis analyze_shell(std::string_view command)
{
    return analyze_shell_impl(command, 0);
}

bool shell_builtin_allowed(std::string_view program)
{
#ifdef _WIN32
    static constexpr std::array<std::string_view, 23> allowed { "cd", "chdir",
        "cls", "dir", "driverquery", "echo", "fc", "find", "findstr", "help",
        "hostname", "more", "netstat", "popd", "pushd", "systeminfo",
        "tasklist", "tree", "type", "ver", "vol", "where", "whoami" };
#else
    // Whole-program read-only commands: no flag or operand can mutate the
    // filesystem, spawn a program, or change remote/privileged state. Safe
    // even as a program-level grant.
    static constexpr std::array<std::string_view, 75> allowed { "arch",
        "base64", "basename", "cat", "cd", "cksum", "cmp", "comm", "cut", "df",
        "diff", "dirname", "du", "echo", "expr", "false", "file", "fold",
        "free", "getent", "grep", "groups", "head", "id", "jq", "join", "last",
        "logname", "ls", "lsblk", "lscpu", "lsof", "md5sum", "netstat", "nl",
        "nm", "nproc", "objdump", "od", "paste", "pgrep", "popd", "printenv",
        "printf", "ps", "pushd", "pwd", "readelf", "readlink", "realpath",
        "rev", "seq", "sha1sum", "sha256sum", "sha512sum", "shasum", "sleep",
        "ss", "stat", "strings", "tac", "tail", "tr", "true", "tty", "type",
        "uname", "uniq", "uptime", "w", "wc", "whereis", "which", "who",
        "whoami" };
#endif
    for (const std::string_view candidate : allowed) {
        if (program == candidate) {
            return true;
        }
    }
    return false;
}

bool shell_deletion_denied(std::string_view program)
{
    // The replacement binding is imza.fs.remove; the evaluator turns this
    // into the model-facing guidance.
    static constexpr std::array<std::string_view, 6> denied { "rm", "rmdir",
        "del", "erase", "unlink", "shred" };
    for (const std::string_view candidate : denied) {
        if (program == candidate) {
            return true;
        }
    }
    return false;
}

bool shell_launcher_denied(std::string_view program)
{
    // The set analyze_shell unwraps: a grant naming one of these would
    // authorize every command the program can run.
    static constexpr std::array<std::string_view, 29> denied { "env", "sudo",
        "doas", "xargs", "nice", "nohup", "time", "stdbuf", "command", "exec",
        "sh", "bash", "zsh", "dash", "ksh", "fish", "cmd", "cmd.exe",
        "powershell", "pwsh", "python", "python3", "perl", "ruby", "node",
        "php", "lua", "awk", "gawk" };
    for (const std::string_view candidate : denied) {
        if (program == candidate) {
            return true;
        }
    }
    return false;
}

bool shell_readonly_allowed(const ShellInvocation& invocation)
{
    if (invocation.program.empty()) {
        return false;
    }
    // Combined short flags (git status -sb) match when every character is an
    // allowed flag; uncombined args must match the catalog verbatim.
    const auto argument_allowed = [](const ReadOnlyCommand& entry,
                                      const std::string& argument) {
        if (argument.size() > 2 && argument.front() == '-'
            && argument[1] != '-') {
            return std::all_of(
                argument.begin() + 1, argument.end(), [&entry](char flag) {
                    return flag_allowed(entry.flags, std::string("-") + flag);
                });
        }
        return flag_allowed(entry.flags, argument);
    };
    const auto matches = [&argument_allowed](const ShellInvocation& call,
                             const ReadOnlyCommand& entry) {
        if (call.program != entry.program
            || call.subcommand != entry.subcommand) {
            return false;
        }
        return std::all_of(call.arguments.begin(), call.arguments.end(),
            [&entry, &argument_allowed](const std::string& argument) {
                return argument_allowed(entry, argument);
            });
    };
#ifdef _WIN32
    // Windows commands keyed on /flag syntax cannot be catalogued: the
    // analyzer only treats '-' as a flag prefix, so '/v' lands in the
    // subcommand slot. Windows relies on the builtin list instead.
    static constexpr std::array<ReadOnlyCommand, 0> platform_only { };
#else
    static constexpr std::array<ReadOnlyCommand, 25> platform_only {
        { { "top", std::nullopt, "-b -n -p -u -d" },
            { "systemctl", "status", "-l --no-pager" },
            { "systemctl", "list-units", "--type --state --all" },
            { "systemctl", "list-timers", "--all" },
            { "systemctl", "is-active", "" }, { "systemctl", "is-enabled", "" },
            { "systemctl", "is-failed", "" }, { "systemctl", "show", "" },
            { "systemctl", "cat", "" },
            { "systemctl", "list-dependencies", "" },
            { "systemctl", "get-default", "" },
            { "dnf", "list",
                "--installed --available --updates "
                "--obsoletes --recent --all" },
            { "dnf", "info", "--installed --available --updates --all" },
            { "dnf", "repolist", "" }, { "dnf", "search", "" },
            { "dnf", "provides", "" }, { "dnf", "check-update", "" },
            { "cargo", "tree", "-i --invert -e --edges -d --duplicates" },
            { "cargo", "metadata", "--no-deps --format-version" },
            { "cargo", "locate-project", "--workspace --message-format" },
            { "gem", "list", "-a --all -l --local -r --remote -d" },
            { "gem", "which", "" }, { "gem", "environment", "" },
            { "gem", "contents", "" }, { "date", std::nullopt, "-u -R -I" } }
    };
#endif
    static constexpr std::array<ReadOnlyCommand, 45> shared {
        { { "git", "status",
              "-s -b -v --short --branch --porcelain -u "
              "--untracked-files" },
            { "git", "log",
                "-p -n --oneline --graph --all --stat --follow "
                "--decorate" },
            { "git", "diff",
                "--cached --stat --name-only --name-status "
                "--shortstat --summary -U --unified" },
            { "git", "show", "--stat --name-only --name-status --oneline" },
            { "git", "blame", "-L -w -M -C --line-porcelain" },
            { "git", "branch",
                "-a -r -v -vv --list --show-current "
                "--contains --merged --no-merged" },
            { "git", "tag",
                "-l -n --list --contains --merged --no-merged "
                "--sort" },
            { "git", "remote", "-v --verbose --get-url" },
            { "git", "ls-files",
                "-s -o -m -c -d --others --modified "
                "--cached --deleted --full-name" },
            { "git", "describe", "--tags --all --long --abbrev --contains" },
            { "git", "config", "--get --get-regexp --list --list-all" },
            { "docker", "ps", "-a -q -s --all --quiet --size" },
            { "docker", "images", "-a -q --all --quiet" },
            { "docker", "logs", "-f -t --follow --timestamps --tail" },
            { "docker", "inspect", "" }, { "docker", "version", "" },
            { "docker", "info", "" }, { "docker", "top", "" },
            { "docker", "port", "" }, { "docker", "stats", "" },
            { "docker", "history", "" }, { "docker", "diff", "" },
            { "docker", "search", "" },
            { "kubectl", "get",
                "-o --output -A --all-namespaces -n "
                "--namespace --watch" },
            { "kubectl", "describe", "-n --namespace" },
            { "kubectl", "logs", "-f --tail -n --namespace --previous" },
            { "kubectl", "api-resources", "" },
            { "kubectl", "api-versions", "" },
            { "kubectl", "cluster-info", "" }, { "kubectl", "explain", "" },
            { "kubectl", "version", "" },
            { "npm", "ls", "-g --global --depth" },
            { "npm", "outdated", "-g --global" }, { "npm", "view", "" },
            { "npm", "why", "" }, { "npm", "root", "" },
            { "npm", "prefix", "" },
            { "pip", "list", "-o --outdated --format" },
            { "pip", "show", "-f --files" }, { "pip", "freeze", "" },
            { "pip", "check", "" }, { "go", "version", "-m" },
            { "go", "env", "" }, { "go", "list", "" }, { "go", "doc", "" } }
    };
    return std::any_of(platform_only.begin(), platform_only.end(),
               [&](const ReadOnlyCommand& entry) {
                   return matches(invocation, entry);
               })
        || std::any_of(
            shared.begin(), shared.end(), [&](const ReadOnlyCommand& entry) {
                return matches(invocation, entry);
            });
}

} // namespace imza
