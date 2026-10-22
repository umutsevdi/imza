#pragma once

// The test tree's platform boundary: the only test header that includes
// POSIX headers or calls setenv/getpid.

#include <filesystem>
#include <memory>
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

// A command that exits 0 immediately, for tests that only need a child to
// succeed: POSIX `true`, the exit-0 fixture on Windows where `true` is not a
// program.
std::string editor_command();

// Prints what the child's stdin is connected to.
std::string stdin_target_command();

// A command that creates `path`. `mkdir` exists on every platform and is in
// no read-only catalogue, so it always reaches the permission gate.
std::string mutating_command(const std::filesystem::path& path);
// The program name mutating_command() runs, for grant assertions.
std::string_view mutating_program();

// Joins two commands with the host shell's separator (POSIX ';', cmd '&').
std::string shell_and(std::string_view first, std::string_view second);

// Renders `path` as a Lua short-string literal: backslashes doubled so a
// Windows path does not trip the \u/\t escape machinery.
std::string lua_path(const std::filesystem::path& path);

// True when a `git` executable answers on PATH; probed once. Repository
// fixtures report a skip instead of a 20-second silent timeout without it.
bool git_available();

// A foreign holder parked on a worker thread for the lifetime of the object.
// The thread matters: Windows named mutexes are recursive per-thread, so a
// lock taken on the calling thread is invisible to foreign_lock_live() and
// every is_locked()-style predicate. release() drops the lock early;
// destruction releases whatever is still held and joins the worker.
class ForeignLock {
public:
    explicit ForeignLock(const std::filesystem::path& file);
    ~ForeignLock();

    void release();

    ForeignLock(const ForeignLock&)            = delete;
    ForeignLock& operator=(const ForeignLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

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
