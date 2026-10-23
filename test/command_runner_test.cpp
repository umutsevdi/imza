#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

#include "platform/command_runner.h"
#include "test_fs.h"
#include "test_platform.h"

using namespace std::chrono_literals;

namespace {

imza::CommandResult run(const std::string& cmd, std::chrono::seconds t)
{
    return imza::run_command(cmd, t);
}

std::string echo_command(const std::string& text)
{
    return imza::test::fixture_command("echo", text);
}
} // namespace

TEST_CASE("run_command merges env over the inherited environment")
{
    // The parent's variables stay visible unless overridden.
    imza::test::ScopedEnv guard("IMZA_TEST_MERGE", "base");
    const auto inherited = imza::run_command(
        imza::test::fixture_command("env", "IMZA_TEST_MERGE"), 10s);
    CHECK(inherited.spawned);
    CHECK(inherited.output.find("base") != std::string::npos);

    std::map<std::string, std::string> overrides;
    overrides["IMZA_TEST_MERGE"] = "overridden";
    overrides["IMZA_TEST_NEW"]   = "added";
    const auto merged            = imza::run_command(
        imza::test::fixture_command("env", "IMZA_TEST_MERGE"), 10s, { }, { },
        imza::StderrCapture::MERGE, overrides);
    CHECK(merged.output.find("overridden") != std::string::npos);

    const auto added
        = imza::run_command(imza::test::fixture_command("env", "IMZA_TEST_NEW"),
            10s, { }, { }, imza::StderrCapture::MERGE, overrides);
    CHECK(added.output.find("added") != std::string::npos);
}

TEST_CASE("run_command captures stdout")
{
    const auto r = run(echo_command("hello-world"), 10s);
    CHECK(r.spawned);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(r.output.find("hello-world") != std::string::npos);
}

TEST_CASE("run_command reports non-zero exit codes")
{
    const auto r = run(imza::test::exit_command(7), 10s);
    CHECK(r.spawned);
    CHECK(r.exit_code == 7);
}

TEST_CASE("run_command combines stderr with stdout")
{
    const auto r = run(imza::test::fixture_command("both"), 10s);
    CHECK(r.spawned);
    CHECK(r.output.find("out") != std::string::npos);
    CHECK(r.output.find("err") != std::string::npos);
}

TEST_CASE("run_command rejects empty command without spawning")
{
    const auto r = run("", 10s);
    CHECK_FALSE(r.spawned);
}

TEST_CASE("run_command does not leak the parent stdin to the child")
{
    // Without an explicit redirect the child would share the terminal stdin
    // with the UI's reader and race it for keystrokes; the fixture reports
    // what its stdin is actually connected to.
    const auto r = run(imza::test::stdin_target_command(), 10s);
    CHECK(r.spawned);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    std::string target = r.output;
    while (
        !target.empty() && (target.back() == '\n' || target.back() == '\r')) {
        target.pop_back();
    }
#ifdef _WIN32
    CHECK(target == "nul");
#else
    CHECK(target == "/dev/null");
#endif
}

TEST_CASE("run_command enforces the timeout")
{
    const auto r = run(imza::test::sleep_seconds(30), 1s);
    CHECK(r.spawned);
    CHECK(r.timed_out);
}

TEST_CASE("run_command pipes stdin_data to the child")
{
    const auto r = imza::run_command(
        imza::test::fixture_command("cat"), 10s, { }, "stdin-payload");
    CHECK(r.spawned);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(imza::test::normalize_newlines(r.output) == "stdin-payload");
}

TEST_CASE("run_command streams stdin larger than the pipe buffer")
{
    const std::string big(256 * 1024, 'x');
    const auto r
        = imza::run_command(imza::test::fixture_command("cat"), 10s, { }, big);
    CHECK(r.spawned);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(imza::test::normalize_newlines(r.output) == big);
}

TEST_CASE("run_command survives a child that exits before reading stdin")
{
    const auto r
        = imza::run_command(imza::test::exit_command(0), 10s, { }, "ignored");
    CHECK(r.spawned);
    CHECK(r.exit_code == 0);
}

TEST_CASE("run_attached_command reports completion")
{
    const auto result = imza::run_attached_command(imza::test::exit_command(7));
    CHECK(result.spawned);
    CHECK(result.exit_code == 7);
    CHECK_FALSE(result.timed_out);
}

TEST_CASE("run_attached_command rejects an empty command")
{
    CHECK_FALSE(imza::run_attached_command("").spawned);
}
