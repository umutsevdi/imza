#include "test_platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace imza::test {

namespace {

    std::filesystem::path& executable_storage()
    {
        static std::filesystem::path value;
        return value;
    }

    // Plain words so the mode survives both shells.
    constexpr std::string_view FIXTURE_FLAG = "--imza-test-fixture";

    std::string quote_for_shell(const std::string& text)
    {
#ifdef _WIN32
        return "\"" + text + "\"";
#else
        std::string quoted = "'";
        for (const char character : text) {
            if (character == '\'') {
                quoted += "'\\''";
            } else {
                quoted += character;
            }
        }
        return quoted + "'";
#endif
    }

} // namespace

void set_env(std::string_view name, std::string_view value)
{
    const std::string key(name);
    const std::string text(value);
#ifdef _WIN32
    _putenv_s(key.c_str(), text.c_str());
#else
    ::setenv(key.c_str(), text.c_str(), 1);
#endif
}

void unset_env(std::string_view name)
{
    const std::string key(name);
#ifdef _WIN32
    _putenv_s(key.c_str(), "");
#else
    ::unsetenv(key.c_str());
#endif
}

std::string env_value(std::string_view name)
{
    const std::string key(name);
#ifdef _WIN32
    char* buffer   = nullptr;
    std::size_t sz = 0;
    if (_dupenv_s(&buffer, &sz, key.c_str()) != 0 || buffer == nullptr) {
        return "";
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(key.c_str());
    return value != nullptr ? std::string(value) : std::string();
#endif
}

ScopedEnv::ScopedEnv(std::string_view name, std::string_view value)
    : _name(name)
{
    const std::string previous = env_value(name);
    if (!previous.empty()) {
        _previous     = previous;
        _had_previous = true;
    }
    set_env(name, value);
}

ScopedEnv::~ScopedEnv()
{
    if (_had_previous) {
        set_env(_name, _previous);
    } else {
        unset_env(_name);
    }
}

long process_id()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

const std::filesystem::path& executable_path() { return executable_storage(); }

void set_executable_path(const char* argv0)
{
    if (argv0 == nullptr || *argv0 == '\0') {
        return;
    }
    std::error_code error;
    std::filesystem::path candidate = std::filesystem::absolute(argv0, error);
    if (error) {
        candidate = argv0;
    }
    std::filesystem::path canonical
        = std::filesystem::weakly_canonical(candidate, error);
    executable_storage() = error ? candidate : canonical;
}

bool run_fixture(int argc, char** argv)
{
    if (argc < 2 || std::string_view(argv[1]) != FIXTURE_FLAG) {
        return false;
    }
    const std::string mode     = argc >= 3 ? argv[2] : "";
    const std::string argument = argc >= 4 ? argv[3] : "";

    if (mode == "cat") {
        // Buffering is fine: the caller reads the captured output only after
        // the child exits.
        std::cout << std::cin.rdbuf();
        return true;
    }
    if (mode == "lines") {
        // Flushed per line: Process::read_line waits on individual lines
        // while the child is still running.
        std::string line;
        while (std::getline(std::cin, line)) {
            std::cout << "got:" << line << '\n' << std::flush;
        }
        return true;
    }
    if (mode == "echo") {
        std::cout << argument << '\n';
        return true;
    }
    if (mode == "sleep") {
        const int seconds = argument.empty() ? 0 : std::atoi(argument.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        return true;
    }
    if (mode == "exit") {
        std::exit(argument.empty() ? 0 : std::atoi(argument.c_str()));
    }
    if (mode == "touch") {
        std::ofstream file(argument, std::ios::binary | std::ios::app);
        return true;
    }
    if (mode == "both") {
        std::cout << "out" << std::endl;
        std::cerr << "err" << std::endl;
        return true;
    }
    if (mode == "env") {
        std::cout << env_value(argument) << '\n';
        return true;
    }
    if (mode == "pwd") {
        std::cout << std::filesystem::current_path().string() << '\n';
        return true;
    }
    if (mode == "stdin-target") {
        // Lets a test prove the parent's terminal stdin was not inherited.
#ifdef _WIN32
        std::cout << "nul\n";
#else
        char buffer[4096];
        const ssize_t length
            = ::readlink("/proc/self/fd/0", buffer, sizeof(buffer) - 1);
        if (length <= 0) {
            std::cout << "unknown\n";
        } else {
            buffer[length] = '\0';
            std::cout << buffer << '\n';
        }
#endif
        return true;
    }
    std::cerr << "unknown fixture mode: " << mode << '\n';
    return true;
}

std::vector<std::string> fixture_argv(
    std::string_view mode, std::string_view argument)
{
    std::vector<std::string> argv { executable_path().string(),
        std::string(FIXTURE_FLAG), std::string(mode) };
    if (!argument.empty()) {
        argv.emplace_back(argument);
    }
    return argv;
}

std::string fixture_command(std::string_view mode, std::string_view argument)
{
    std::string command = quote_for_shell(executable_path().string()) + " "
        + std::string(FIXTURE_FLAG) + " " + std::string(mode);
    if (!argument.empty()) {
        command += " " + quote_for_shell(std::string(argument));
    }
    return command;
}

std::string normalize_newlines(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;
            }
            out += '\n';
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string sleep_seconds(int seconds)
{
    return fixture_command("sleep", std::to_string(seconds));
}

std::string exit_command(int code)
{
    return fixture_command("exit", std::to_string(code));
}

std::string print_cwd_command()
{
    // Both print the working directory and are catalogued read-only, so the
    // gate accepts them without an approval.
#ifdef _WIN32
    return "cd";
#else
    return "pwd";
#endif
}

std::string failing_command()
{
#ifdef _WIN32
    return "where imza-no-such-program";
#else
    return "false";
#endif
}

std::string echo_command(std::string_view text)
{
    return "echo " + std::string(text);
}

std::string stdin_target_command() { return fixture_command("stdin-target"); }

std::string mutating_command(const std::filesystem::path& path)
{
    return "mkdir " + quote_for_shell(path.string());
}

std::string_view mutating_program() { return "mkdir"; }

std::string shell_and(std::string_view first, std::string_view second)
{
#ifdef _WIN32
    return std::string(first) + " & " + std::string(second);
#else
    return std::string(first) + "; " + std::string(second);
#endif
}

bool can_create_symlinks()
{
    static const bool supported = [] {
        std::error_code error;
        const std::filesystem::path root
            = std::filesystem::temp_directory_path(error);
        if (error) {
            return false;
        }
        const std::filesystem::path probe
            = root / ("imza-symlink-probe-" + std::to_string(process_id()));
        std::filesystem::remove_all(probe, error);
        std::filesystem::create_directories(probe / "target", error);
        std::filesystem::create_directory_symlink(
            probe / "target", probe / "link", error);
        const bool ok = !error;
        std::filesystem::remove_all(probe, error);
        return ok;
    }();
    return supported;
}

} // namespace imza::test
