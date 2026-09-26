#include <filesystem>
#include <fstream>
#include <vector>

#include <doctest/doctest.h>
#include <json/json.h>

#include "platform/json_file.h"

namespace {

struct TempDir {
    std::filesystem::path path
        = std::filesystem::temp_directory_path() / "imza_json_file_test";

    TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("write then read_json_file round-trips a document")
{
    const TempDir dir;
    const auto path = dir.path / "store" / "data.json";
    Json::Value root;
    root["version"] = 1;
    root["name"]    = "imza";
    REQUIRE(imza::write_json_file(path, root, "") == imza::Status::OK);

    const auto loaded = imza::read_json_file(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->get("version", 0).asInt() == 1);
    CHECK(loaded->get("name", "").asString() == "imza");
}

TEST_CASE("read_json_file reports missing and malformed documents as absent")
{
    const TempDir dir;
    CHECK_FALSE(imza::read_json_file(dir.path / "missing.json").has_value());

    const auto malformed = dir.path / "malformed.json";
    {
        std::ofstream file(malformed, std::ios::binary);
        file << "{\"version\": 1,";
    }
    CHECK_FALSE(imza::read_json_file(malformed).has_value());
}

TEST_CASE("read_text_file returns exact content or nothing")
{
    const TempDir dir;
    const auto path = dir.path / "text.txt";
    {
        std::ofstream file(path, std::ios::binary);
        file << "line one\nline two";
    }
    const auto content = imza::read_text_file(path);
    REQUIRE(content.has_value());
    CHECK(*content == "line one\nline two");
    CHECK_FALSE(imza::read_text_file(dir.path / "missing.txt").has_value());
}

TEST_CASE("mutate_json_file creates the document from an absent file")
{
    const TempDir dir;
    const auto path = dir.path / "store" / "history.json";
    Json::Value seen;
    const imza::Status status
        = imza::mutate_json_file(path, [&](Json::Value& root) {
              seen            = root;
              root["entries"] = Json::Value(Json::arrayValue);
              root["entries"].append("first");
              return true;
          });
    REQUIRE(status == imza::Status::OK);
    CHECK(seen.isObject());
    CHECK(seen.empty());

    const auto loaded = imza::read_json_file(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->get("entries", Json::Value()).size() == 1);
    CHECK(loaded->get("entries", Json::Value())[0].asString() == "first");
}

TEST_CASE("mutate_json_file mutates the newest stored content")
{
    const TempDir dir;
    const auto path = dir.path / "history.json";
    Json::Value initial;
    initial["version"]    = 1;
    initial["entries"]    = Json::Value(Json::arrayValue);
    initial["entries"][0] = "one";
    REQUIRE(imza::write_json_file(path, initial, "") == imza::Status::OK);

    std::vector<std::string> observed;
    REQUIRE(imza::mutate_json_file(path, [&](Json::Value& root) {
        for (const auto& entry : root["entries"]) {
            observed.push_back(entry.asString());
        }
        root["entries"].append("two");
        return true;
    }) == imza::Status::OK);

    CHECK(observed == std::vector<std::string> { "one" });
    const auto loaded = imza::read_json_file(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->get("version", 0).asInt() == 1);
    CHECK(loaded->get("entries", Json::Value()).size() == 2);
}

TEST_CASE("mutate_json_file leaves the file untouched when unchanged")
{
    const TempDir dir;
    const auto path = dir.path / "history.json";
    Json::Value initial;
    initial["version"] = 1;
    REQUIRE(imza::write_json_file(path, initial, "") == imza::Status::OK);
    const auto before = imza::read_text_file(path);

    REQUIRE(imza::mutate_json_file(path, [](Json::Value&) { return false; })
        == imza::Status::OK);
    CHECK(imza::read_text_file(path) == before);
}
