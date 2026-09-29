#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "platform/command_runner.h"

using namespace std::chrono_literals;

namespace {

imza::CommandResult run(const std::string& cmd, std::chrono::seconds t)
{
    return imza::run_command(cmd, t);
}

} // namespace

TEST_CASE("run_command captures stdout")
{
    const auto r = run("echo hello-world", 10s);
    CHECK(r.spawned);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(r.output.find("hello-world") != std::string::npos);
}

TEST_CASE("run_command reports non-zero exit codes")
{
    const auto r = run("exit 7", 10s);
    CHECK(r.spawned);
    CHECK(r.exit_code == 7);
}

TEST_CASE("run_command combines stderr with stdout")
{
    const auto r = run("echo out; echo err 1>&2", 10s);
    CHECK(r.spawned);
    CHECK(r.output.find("out") != std::string::npos);
    CHECK(r.output.find("err") != std::string::npos);
}

TEST_CASE("run_command rejects empty command without spawning")
{
    const auto r = run("", 10s);
    CHECK_FALSE(r.spawned);
}

TEST_CASE("run_command enforces the timeout")
{
    const auto r = run("sleep 30", 1s);
    CHECK(r.spawned);
    CHECK(r.timed_out);
}

TEST_CASE("run_command pipes stdin_data to the child")
{
#ifdef _WIN32
    const auto r = imza::run_command("findstr x", 10s, { }, "stdin-payload");
    CHECK(r.spawned);
    CHECK(r.exit_code == 0);
#else
    const auto r = imza::run_command("cat", 10s, { }, "stdin-payload");
    CHECK(r.spawned);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(r.output == "stdin-payload");
#endif
}

TEST_CASE("run_command streams stdin larger than the pipe buffer")
{
    const std::string big(256 * 1024, 'x');
    const auto r = imza::run_command("cat", 10s, { }, big);
    CHECK(r.spawned);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(r.output == big);
}

TEST_CASE("run_command survives a child that exits before reading stdin")
{
    const auto r = imza::run_command("true", 10s, { }, "ignored");
    CHECK(r.spawned);
    CHECK(r.exit_code == 0);
}

TEST_CASE("run_attached_command reports completion")
{
#ifdef _WIN32
    const auto result = imza::run_attached_command("cmd.exe /c exit 7");
#else
    const auto result = imza::run_attached_command("exit 7");
#endif
    CHECK(result.spawned);
    CHECK(result.exit_code == 7);
    CHECK_FALSE(result.timed_out);
}

TEST_CASE("run_attached_command rejects an empty command")
{
    CHECK_FALSE(imza::run_attached_command("").spawned);
}
