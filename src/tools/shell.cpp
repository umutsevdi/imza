#include "tools/tool.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
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

    struct CommandPair {
        std::string_view program;
        std::string_view subcommand;
    };

    void append_invocation(
        ShellAnalysis& analysis, std::vector<std::string>& words)
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
        ShellInvocation invocation;
        invocation.program = std::move(words[program]);
#ifdef _WIN32
        std::ranges::transform(invocation.program, invocation.program.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
#endif
        // Subcommand identity: first word after the program that is not an
        // option. A short option such as -C also consumes its separate value.
        std::size_t index = program + 1;
        while (index < words.size() && !words[index].empty()
            && words[index].front() == '-') {
            const bool short_option
                = words[index].size() == 2 && words[index][1] != '-';
            if (short_option && index + 1 < words.size()
                && !words[index + 1].empty()
                && words[index + 1].front() != '-') {
                std::swap(words[index], words[index + 1]);
            }
            ++index;
        }
        if (index < words.size()) {
            invocation.subcommand = std::move(words[index]);
        }
        for (std::size_t rest = program + 1; rest < words.size(); ++rest) {
            invocation.arguments.push_back(std::move(words[rest]));
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

    bool matches_readonly(std::span<const ReadOnlyCommand> commands,
        const ShellInvocation& invocation)
    {
        for (const ReadOnlyCommand& entry : commands) {
            if (entry.program != invocation.program
                || entry.subcommand != invocation.subcommand) {
                continue;
            }
            bool covered = true;
            for (const std::string& argument : invocation.arguments) {
                if (!flag_allowed(entry.flags, argument)) {
                    covered = false;
                    break;
                }
            }
            if (covered) {
                return true;
            }
        }
        return false;
    }

    bool is_separator(char character)
    {
        return character == ';' || character == '|' || character == '&'
            || character == '\n';
    }

} // namespace

ShellAnalysis analyze_shell(std::string_view command)
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
                escaped = true;
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
            quote        = character == '\'' ? Quote::SINGLE : Quote::DOUBLE;
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
            append_invocation(analysis, words);
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
            append_invocation(analysis, words);
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
    append_invocation(analysis, words);
    if (analysis.invocations.empty()) {
        analysis.reuse = ShellAnalysis::Reuse::ONCE;
    }
    return analysis;
}

bool shell_builtin_allowed(std::string_view program)
{
#ifdef _WIN32
    static constexpr std::array<std::string_view, 21> allowed { "cd", "chdir",
        "cls", "dir", "driverquery", "echo", "fc", "find", "findstr", "help",
        "hostname", "more", "popd", "pushd", "systeminfo", "tasklist", "tree",
        "type", "ver", "vol", "whoami" };
#else
    static constexpr std::array<std::string_view, 31> allowed { "basename",
        "cat", "cd", "cmp", "cut", "dirname", "echo", "false", "file", "grep",
        "groups", "head", "id", "ls", "md5sum", "popd", "printenv", "printf",
        "pushd", "pwd", "readlink", "realpath", "sha256sum", "shasum", "stat",
        "strings", "tail", "tr", "true", "uname", "wc" };
#endif
    for (const std::string_view candidate : allowed) {
        if (program == candidate) {
            return true;
        }
    }
    return false;
}

bool shell_readonly_allowed(const ShellInvocation& invocation)
{
    if (!invocation.subcommand || invocation.subcommand->empty()) {
        return false;
    }
#ifdef _WIN32
    static constexpr std::array<ReadOnlyCommand, 12> platform_only {
        { { "tasklist", std::nullopt, "/v /fo /fi /nh" },
            { "driverquery", std::nullopt, "/v /fo /si" },
            { "where", std::nullopt, "/r /f /t /q" },
            { "systeminfo", std::nullopt, "/fo /nh" },
            { "ipconfig", std::nullopt, "/all /displaydns" },
            { "netstat", std::nullopt, "-a -n -o -r -s" },
            { "tree", std::nullopt, "/f /a" },
            { "fc", std::nullopt, "/n /c /l /a" },
            { "findstr", std::nullopt, "/i /v /n /s /r /l" },
            { "wmic", std::nullopt, "/format" }, { "ver", std::nullopt, "" },
            { "whoami", std::nullopt, "/all /groups /priv" } }
    };
#else
    static constexpr std::array<ReadOnlyCommand, 18> platform_only {
        { { "top", std::nullopt, "-b -n -p -u -d" },
            { "ps", std::nullopt, "aux -ef -e -f -u -p --sort" },
            { "df", std::nullopt, "-h -T -i -k -m --output" },
            { "du", std::nullopt, "-s -h -k -m --max-depth" },
            { "free", std::nullopt, "-h -m -g -b -t -w" },
            { "lsof", std::nullopt, "-i -p -u -t -n -P" },
            { "systemctl", "status", "-l --no-pager" },
            { "systemctl", "list-units", "--type --state --all" },
            { "systemctl", "list-timers", "--all" },
            { "dnf", "list",
                "--installed --available --updates "
                "--obsoletes --recent --all" },
            { "dnf", "info", "--installed --available --updates --all" },
            { "whois", "domain", "" },
            { "cargo", "tree", "-i --invert -e --edges -d --duplicates" },
            { "terraform", "plan", "-no-color -input=false -refresh=false" },
            { "gem", "list", "-a --all -l --local -r --remote -d" },
            { "uname", std::nullopt, "-a -r -s -n -v -m -o -p" },
            { "date", std::nullopt, "-u -R -I" },
            { "uptime", std::nullopt, "-p -s" } }
    };
#endif
    // Shared read-only combinations available on every platform.
    static constexpr std::array<ReadOnlyCommand, 26> shared {
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
            { "docker", "inspect", "" },
            { "kubectl", "get",
                "-o --output -A --all-namespaces -n "
                "--namespace --watch" },
            { "kubectl", "describe", "-n --namespace" },
            { "kubectl", "logs", "-f --tail -n --namespace --previous" },
            { "make", std::nullopt, "-n --dry-run" },
            { "which", std::nullopt, "-a" },
            { "npm", "ls", "-g --global --depth" },
            { "npm", "outdated", "-g --global" }, { "npm", "view", "" },
            { "pip", "list", "-o --outdated --format" },
            { "pip", "show", "-f --files" }, { "go", "version", "-m" } }
    };
    return matches_readonly(platform_only, invocation)
        || matches_readonly(shared, invocation);
}

} // namespace imza
