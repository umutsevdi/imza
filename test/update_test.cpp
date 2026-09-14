#include <doctest/doctest.h>

#include <json/json.h>

#include <filesystem>
#include <string>
#include <vector>

#include "platform/json_file.h"
#include "platform/update.h"

namespace {

TEST_CASE("compare_versions orders semantic versions")
{
    using imza::compare_versions;
    CHECK(compare_versions("0.2.2", "0.2.2") == 0);
    CHECK(compare_versions("v0.2.2", "0.2.2") == 0);
    CHECK(compare_versions("imza-0.2.2", "0.2.2") == 0);
    CHECK(compare_versions("0.2.2-1", "0.2.2") == 0);
    CHECK(compare_versions("0.2.2", "0.2.10") < 0);
    CHECK(compare_versions("0.2", "0.2.0") == 0);
    CHECK(compare_versions("0.10.0", "0.9.0") > 0);
    CHECK(compare_versions("1.0.0", "0.99.99") > 0);
    CHECK(compare_versions("0.3", "0.2.9") > 0);
    CHECK(compare_versions("v0.3.0", "0.2.2") > 0);
}

TEST_CASE("expected_asset_name merges the version into the platform asset")
{
    using imza::expected_asset_name;
#if defined(_WIN32)
    CHECK(expected_asset_name({ }, "v0.3.1") == "imza-0.3.1-windows-x64.exe");
#elif defined(__APPLE__)
    const std::string pkg = expected_asset_name({ }, "v0.3.1");
    CHECK(pkg == "imza-0.3.1-macos-x64.pkg"
        || pkg == "imza-0.3.1-macos-arm64.pkg");
    CHECK(expected_asset_name({ "apt" }, "0.3.1")
        == expected_asset_name({ }, "0.3.1"));
#else
    const std::string deb = expected_asset_name({ "apt" }, "v0.3.1");
    CHECK((deb == "imza-0.3.1-linux-x64.deb"
        || deb == "imza-0.3.1-linux-arm64.deb"));
    const std::string rpm = expected_asset_name({ "dnf" }, "0.3.1");
    CHECK((rpm == "imza-0.3.1-linux-x64.rpm"
        || rpm == "imza-0.3.1-linux-arm64.rpm"));
    CHECK(expected_asset_name({ "apt-get" }, "0.3.1") == deb);
    CHECK(expected_asset_name({ "yum" }, "0.3.1") == rpm);
    CHECK(expected_asset_name({ "pacman", "zypper" }, "0.3.1").empty());
    CHECK(expected_asset_name({ }, "0.3.1").empty());
#endif
}

TEST_CASE("cached_update reads the update.json state file")
{
    const std::filesystem::path data
        = std::filesystem::temp_directory_path() / "imza-update-test" / "imza";
    std::filesystem::create_directories(data);
    setenv("XDG_DATA_HOME", data.parent_path().string().c_str(), 1);

    CHECK_FALSE(imza::cached_update("0.2.2").has_value());

    Json::Value root(Json::objectValue);
    root["last_checked_at"] = static_cast<Json::Int64>(1);
    root["version"]         = "9.9.9";
    REQUIRE(imza::write_json_file(data / "update.json", root, "")
        == imza::Status::OK);
    const std::optional<std::string> version = imza::cached_update("0.2.2");
    REQUIRE(version.has_value());
    CHECK(*version == "9.9.9");
    CHECK_FALSE(imza::cached_update("9.9.9").has_value());

    unsetenv("XDG_DATA_HOME");
    std::error_code error;
    std::filesystem::remove_all(data.parent_path(), error);
}

} // namespace
