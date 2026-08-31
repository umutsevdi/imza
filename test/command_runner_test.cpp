#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "command_runner.h"

using namespace std::chrono_literals;

namespace {

ursa::CommandResult run(const std::string& cmd, std::chrono::seconds t)
{
    return ursa::run_command(cmd, t);
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
