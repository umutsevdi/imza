#include <doctest/doctest.h>

#include <string_view>

#include "platform/clipboard.h"

// Clipboard-mutating cases are intentionally untested: tests must not
// overwrite a developer's real clipboard. Only the guards are exercised.

TEST_CASE("copy_to_clipboard rejects empty text without spawning a tool")
{
    CHECK_FALSE(imza::copy_to_clipboard("cat", ""));
}

TEST_CASE("copy_to_clipboard fails cleanly when no tool is available")
{
    CHECK_FALSE(imza::copy_to_clipboard("", "some text"));
}
