#include <vector>

#include <doctest/doctest.h>

#include "network/json.h"
#include "platform/json_file.h"
#include "test_fs.h"

namespace {

using imza::test::TempDir;
using JsonValue = imza::JsonValue;

} // namespace

TEST_CASE("write then read_json_file round-trips a document")
{
    const TempDir dir;
    const auto path      = dir.path / "store" / "data.json";
    imza::JsonValue root = JsonValue::object_t { };
    {
        auto& object      = root.get<JsonValue::object_t>();
        object["version"] = JsonValue(1.0);
        object["name"]    = JsonValue("imza");
    }
    REQUIRE(
        imza::write_json_file(path, imza::json_dump(root)) == imza::Status::OK);

    const auto loaded = imza::read_json_file(path);
    REQUIRE(loaded.has_value());
    const imza::JsonValue* version = imza::find_member(*loaded, "version");
    REQUIRE(version != nullptr);
    REQUIRE(version->is_number());
    const int stored_version = version->as<int>();
    CHECK(stored_version == 1);
    const imza::JsonValue* name = imza::find_member(*loaded, "name");
    REQUIRE(name != nullptr);
    REQUIRE(name->is_string());
    const std::string stored_name = name->as<std::string>();
    CHECK(stored_name == "imza");
}

TEST_CASE("read_json_file reports missing and malformed documents as absent")
{
    const TempDir dir;
    CHECK_FALSE(imza::read_json_file(dir.path / "missing.json").has_value());

    const auto malformed = dir.path / "malformed.json";
    imza::test::write_file(malformed, "{\"version\": 1,");
    CHECK_FALSE(imza::read_json_file(malformed).has_value());
}

TEST_CASE("read_text_file returns exact content or nothing")
{
    const TempDir dir;
    const auto path = dir.path / "text.txt";
    imza::test::write_file(path, "line one\nline two");
    const auto content = imza::read_text_file(path);
    REQUIRE(content.has_value());
    CHECK(*content == "line one\nline two");
    CHECK_FALSE(imza::read_text_file(dir.path / "missing.txt").has_value());
}

TEST_CASE("mutate_json_file creates the document from an absent file")
{
    const TempDir dir;
    const auto path = dir.path / "store" / "history.json";
    imza::JsonValue seen;
    const imza::Status status
        = imza::mutate_json_file(path, [&](imza::JsonValue& root) {
              seen              = root;
              JsonValue entries = JsonValue::array_t { };
              entries.get<JsonValue::array_t>().emplace_back("first");
              root.get<JsonValue::object_t>()["entries"] = std::move(entries);
              return true;
          });
    REQUIRE(status == imza::Status::OK);
    CHECK(seen.is_object());
    CHECK(seen.get<JsonValue::object_t>().empty());

    const auto loaded = imza::read_json_file(path);
    REQUIRE(loaded.has_value());
    const imza::JsonValue* entries = imza::find_member(*loaded, "entries");
    REQUIRE(entries != nullptr);
    REQUIRE(entries->is_array());
    const auto& list = entries->get<JsonValue::array_t>();
    CHECK(list.size() == 1);
    REQUIRE(!list.empty());
    CHECK(list[0].as<std::string>() == "first");
}

TEST_CASE("mutate_json_file mutates the newest stored content")
{
    const TempDir dir;
    const auto path         = dir.path / "history.json";
    imza::JsonValue initial = JsonValue::object_t { };
    {
        auto& object      = initial.get<JsonValue::object_t>();
        object["version"] = JsonValue(1.0);
        JsonValue entries = JsonValue::array_t { };
        entries.get<JsonValue::array_t>().emplace_back("one");
        object["entries"] = std::move(entries);
    }
    REQUIRE(imza::write_json_file(path, imza::json_dump(initial))
        == imza::Status::OK);

    std::vector<std::string> observed;
    REQUIRE(imza::mutate_json_file(path, [&](imza::JsonValue& root) {
        const imza::JsonValue* entries = imza::find_member(root, "entries");
        if (entries != nullptr && entries->is_array()) {
            for (const imza::JsonValue& entry :
                entries->get<JsonValue::array_t>()) {
                observed.push_back(entry.as<std::string>());
            }
        }
        imza::JsonValue& list = root.get<JsonValue::object_t>()["entries"];
        list.get<JsonValue::array_t>().emplace_back("two");
        return true;
    }) == imza::Status::OK);

    CHECK(observed == std::vector<std::string> { "one" });
    const auto loaded = imza::read_json_file(path);
    REQUIRE(loaded.has_value());
    const imza::JsonValue* version = imza::find_member(*loaded, "version");
    REQUIRE(version != nullptr);
    REQUIRE(version->is_number());
    CHECK(version->as<int>() == 1);
    const imza::JsonValue* entries = imza::find_member(*loaded, "entries");
    REQUIRE(entries != nullptr);
    REQUIRE(entries->is_array());
    CHECK(entries->get<JsonValue::array_t>().size() == 2);
}

TEST_CASE("mutate_json_file leaves the file untouched when unchanged")
{
    const TempDir dir;
    const auto path                               = dir.path / "history.json";
    imza::JsonValue initial                       = JsonValue::object_t { };
    initial.get<JsonValue::object_t>()["version"] = JsonValue(1.0);
    REQUIRE(imza::write_json_file(path, imza::json_dump(initial))
        == imza::Status::OK);
    const auto before = imza::read_text_file(path);

    REQUIRE(imza::mutate_json_file(path, [](JsonValue&) { return false; })
        == imza::Status::OK);
    CHECK(imza::read_text_file(path) == before);
}
