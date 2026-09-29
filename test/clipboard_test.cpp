#include <doctest/doctest.h>

#include <string_view>

#include "platform/clipboard.h"

namespace {

imza::SystemEnvironment environment_with(std::string_view tool)
{
    imza::SystemEnvironment environment;
    environment.clipboard_tool = std::string(tool);
    return environment;
}

} // namespace

// Clipboard-mutating cases are intentionally untested: tests must not
// overwrite a developer's real clipboard. Only the guards are exercised.

TEST_CASE("copy_to_clipboard rejects empty text without spawning a tool")
{
    const auto environment = environment_with("cat");
    CHECK_FALSE(imza::copy_to_clipboard(environment, ""));
}

TEST_CASE("copy_to_clipboard fails cleanly when no tool is available")
{
    const auto environment = environment_with("");
    CHECK(environment.clipboard_tool.empty());
    CHECK_FALSE(imza::copy_to_clipboard(environment, "some text"));
}
