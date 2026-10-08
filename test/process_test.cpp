#include <doctest/doctest.h>

#include "platform/process.h"

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

#ifdef _WIN32
// The fixtures below are POSIX shells.
constexpr bool POSIX_FIXTURES = false;
#else
constexpr bool POSIX_FIXTURES = true;
#endif

imza::ProcessOptions shell_options(std::string script)
{
    imza::ProcessOptions options;
    options.argv            = { "/bin/sh", "-c", std::move(script) };
    options.terminate_grace = 500ms;
    return options;
}

} // namespace

TEST_CASE("process spawn rejects an empty command without spawning")
{
    std::string error;
    imza::ProcessOptions options;
    CHECK(imza::Process::spawn(options, error) == nullptr);
    CHECK(error.find("empty") != std::string::npos);
}

TEST_CASE("process round-trips a line through stdin and stdout")
{
    if (!POSIX_FIXTURES) {
        return;
    }
    std::string error;
    // Echo each stdin line back, prefixed; exit on EOF.
    auto process = imza::Process::spawn(
        shell_options("while IFS= read -r line; do printf 'got:%s\\n' "
                      "\"$line\"; done"),
        error);
    REQUIRE(process != nullptr);
    REQUIRE(process->write_line("hello") == imza::Status::OK);

    std::string line;
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "got:hello");

    process->terminate();
    CHECK_FALSE(process->running());
}

TEST_CASE("process captures stderr separately from stdout")
{
    if (!POSIX_FIXTURES) {
        return;
    }
    std::string error;
    auto process = imza::Process::spawn(
        shell_options("echo out; echo err 1>&2; echo out2"), error);
    REQUIRE(process != nullptr);

    std::string line;
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "out");
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "out2");

    // Let the stderr reader drain, then confirm it never leaked into stdout.
    for (int i = 0; i < 100 && process->running(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(process->stderr_tail().find("err") != std::string::npos);
}

TEST_CASE("process read_line times out when the child stays silent")
{
    if (!POSIX_FIXTURES) {
        return;
    }
    std::string error;
    auto process = imza::Process::spawn(shell_options("sleep 5"), error);
    REQUIRE(process != nullptr);

    std::string line;
    CHECK(process->read_line(line, 200ms) == imza::Status::TIMEOUT);
    process->terminate();
}

TEST_CASE("process terminate reaps the child")
{
    if (!POSIX_FIXTURES) {
        return;
    }
    std::string error;
    auto process = imza::Process::spawn(shell_options("sleep 30"), error);
    REQUIRE(process != nullptr);
    CHECK(process->running());

    process->terminate();
    CHECK_FALSE(process->running());
    // Idempotent.
    process->terminate();
    CHECK_FALSE(process->running());
}

TEST_CASE("process env overrides reach the child")
{
    if (!POSIX_FIXTURES) {
        return;
    }
    std::string error;
    imza::ProcessOptions options = shell_options("printf '%s\\n' \"$IMZA_X\"");
    options.env["IMZA_X"]        = "custom-value";
    auto process                 = imza::Process::spawn(options, error);
    REQUIRE(process != nullptr);

    std::string line;
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "custom-value");
}
