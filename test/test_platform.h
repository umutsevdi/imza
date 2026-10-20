#pragma once

// The test tree's platform boundary: the only test header that includes
// POSIX headers or calls setenv/getpid.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

namespace imza::test {

// Windows has no setenv, so every environment write goes through these.
void set_env(std::string_view name, std::string_view value);
void unset_env(std::string_view name);
// Empty when unset, which is also how an empty value reads.
std::string env_value(std::string_view name);

// RAII restore of one variable: exports a value on construction and puts the
// previous state back on destruction, including when an assertion throws.
class ScopedEnv {
public:
    ScopedEnv(std::string_view name, std::string_view value);
    ~ScopedEnv();

    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string _name;
    std::string _previous;
    bool _had_previous = false;
};

long process_id();

// Absolute path of the running test binary, captured once in main().
const std::filesystem::path& executable_path();
void set_executable_path(const char* argv0);

// The test binary doubles as a portable child process: these modes replace
// the POSIX shell fixtures (cat, sleep, printf, readlink) the suite used to
// assume. run_fixture() handles them before doctest parses argv.
bool run_fixture(int argc, char** argv);

// argv for Process::spawn (passed verbatim, no shell).
std::vector<std::string> fixture_argv(
    std::string_view mode, std::string_view argument = { });

// Shell command line invoking the fixture, for run_command()/imza.shell.
std::string fixture_command(
    std::string_view mode, std::string_view argument = { });

// Normalizes CRLF and bare CR to LF: Windows children emit CRLF, and
// assertions compare text, not line-ending convention.
std::string normalize_newlines(std::string text);

std::string sleep_seconds(int seconds);
std::string exit_command(int code);

// Catalogued read-only on both hosts, so the permission gate accepts it
// without an approval.
std::string print_cwd_command();

// Exits non-zero because the name is never found; catalogued read-only, so
// it never prompts.
std::string failing_command();

// Catalogued read-only on both hosts.
std::string echo_command(std::string_view text);

// Prints what the child's stdin is connected to.
std::string stdin_target_command();

// A command that creates `path`. `mkdir` exists on every platform and is in
// no read-only catalogue, so it always reaches the permission gate.
std::string mutating_command(const std::filesystem::path& path);
// The program name mutating_command() runs, for grant assertions.
std::string_view mutating_program();

// Joins two commands with the host shell's separator (POSIX ';', cmd '&').
std::string shell_and(std::string_view first, std::string_view second);

// Symlink creation needs a privilege Windows only grants in developer mode.
// Probed once; tests that need a real symlink report a skip instead of
// failing on a machine that cannot create one.
bool can_create_symlinks();

// For a genuinely unsupported platform case, never to paper over a
// difference the platform layer already handles.
#define SKIP_TEST(reason)                                                      \
    do {                                                                       \
        MESSAGE("skipped: " << (reason));                                      \
        return;                                                                \
    } while (false)

} // namespace imza::test
