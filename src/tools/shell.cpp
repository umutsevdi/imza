#include "tools/tool.h"

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
        if (program + 1 < words.size()) {
            invocation.subcommand = std::move(words[program + 1]);
        }
        analysis.invocations.push_back(std::move(invocation));
        words.clear();
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
    static constexpr std::array<std::string_view, 20> allowed { "cd", "chdir",
        "cls", "dir", "driverquery", "echo", "fc", "find", "findstr", "help",
        "hostname", "more", "systeminfo", "tasklist", "tree", "type", "ver",
        "vol", "where", "whoami" };
#else
    static constexpr std::array<std::string_view, 27> allowed { "basename",
        "cat", "cmp", "cut", "dirname", "echo", "false", "file", "grep",
        "groups", "head", "id", "ls", "md5sum", "printenv", "printf", "pwd",
        "readlink", "realpath", "sha256sum", "stat", "strings", "tail", "tr",
        "true", "uname", "wc" };
#endif
    for (const std::string_view candidate : allowed) {
        if (program == candidate) {
            return true;
        }
    }
    return false;
}

} // namespace imza
