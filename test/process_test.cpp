#include <doctest/doctest.h>

#include "platform/process.h"
#include "test_platform.h"

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

// The test binary re-runs itself as the child (test_platform.h): Process
// passes argv verbatim with no shell, so a fixture needs no host shell.
imza::ProcessOptions fixture_options(
    std::string mode, std::string argument = { })
{
    imza::ProcessOptions options;
    options.argv = imza::test::fixture_argv(mode, std::move(argument));
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
    std::string error;
    auto process = imza::Process::spawn(fixture_options("lines"), error);
    REQUIRE(process != nullptr);
    REQUIRE(process->write_line("hello") == imza::Status::OK);
    REQUIRE(process->write_line("second") == imza::Status::OK);

    std::string line;
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "got:hello");
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "got:second");
    // read_line strips the child's CRLF, so a Windows host reads clean
    // lines like a POSIX one.

    process->terminate();
    CHECK_FALSE(process->running());
}

TEST_CASE("process captures stderr separately from stdout")
{
    std::string error;
    // The fixture writes to stdout only; stderr must stay empty rather than
    // mirroring the child's output.
    auto process = imza::Process::spawn(fixture_options("echo", "out"), error);
    REQUIRE(process != nullptr);

    std::string line;
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "out");
    CHECK(process->stderr_tail().find("out") == std::string::npos);
}

TEST_CASE("process read_line times out when the child stays silent")
{
    std::string error;
    auto process = imza::Process::spawn(fixture_options("sleep", "5"), error);
    REQUIRE(process != nullptr);

    std::string line;
    CHECK(process->read_line(line, 200ms) == imza::Status::TIMEOUT);
    process->terminate();
}

TEST_CASE("process terminate reaps the child")
{
    std::string error;
    auto process = imza::Process::spawn(fixture_options("sleep", "30"), error);
    REQUIRE(process != nullptr);
    CHECK(process->running());

    process->terminate();
    CHECK_FALSE(process->running());
    process->terminate();
    CHECK_FALSE(process->running());
}

TEST_CASE("process env overrides reach the child")
{
    std::string error;
    imza::ProcessOptions options = fixture_options("env", "IMZA_X");
    options.env["IMZA_X"]        = "custom-value";
    auto process                 = imza::Process::spawn(options, error);
    REQUIRE(process != nullptr);

    std::string line;
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK(line == "custom-value");
}

TEST_CASE("process env overrides merge over the inherited environment")
{
    // The child environment is built from scratch on every host; an override
    // must not drop the rest, or PATH disappears and later spawns fail.
    std::string error;
    imza::ProcessOptions options = fixture_options("env", "PATH");
    options.env["IMZA_X"]        = "custom-value";
    auto process                 = imza::Process::spawn(options, error);
    REQUIRE(process != nullptr);

    std::string line;
    REQUIRE(process->read_line(line, 5s) == imza::Status::OK);
    CHECK_FALSE(line.empty());
    process->terminate();
}
