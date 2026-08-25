#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <string>

#include "environment.h"

namespace {

    TEST_CASE("analyze_environment populates the core fields")
    {
        const auto e = ursa::analyze_environment();
        CHECK_FALSE(e.os_name.empty());
        CHECK_FALSE(e.default_shell.empty());
        CHECK_FALSE(e.today.empty());
        CHECK(e.today.size() == 10);
        CHECK(e.today[4] == '-');
        CHECK(e.today[7] == '-');
    }

    TEST_CASE("analyze_environment_async resolves to a ready future")
    {
        auto future = ursa::analyze_environment_async();
        REQUIRE(future.valid());
        CHECK(future.wait_for(std::chrono::seconds(5))
            == std::future_status::ready);
        const auto e = future.get();
        CHECK_FALSE(e.os_name.empty());
    }

} // namespace
