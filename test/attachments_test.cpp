#include <algorithm>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include "attachments.h"
#include "session.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() / "ursa_attachment_test";

    TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path / "src");
        fs::create_directories(path / "node_modules");
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void write(const fs::path& relative, std::string_view content)
    {
        std::ofstream file(path / relative, std::ios::binary);
        file << content;
    }
};

} // namespace

TEST_CASE("attachment token is recognized only at a token boundary")
{
    auto token = ursa::attachment_token_at("review @src/ma", 14);
    REQUIRE(token);
    CHECK(token->query == "src/ma");
    CHECK_FALSE(ursa::attachment_token_at("me@example.com", 14));
}

TEST_CASE("attachment candidates list one directory without large directories")
{
    TempDir tmp;
    tmp.write("src/main.cpp", "int main() {}\n");
    tmp.write("readme.md", "hello\n");

    const auto root = ursa::attachment_candidates(tmp.path, "");
    CHECK(std::none_of(root.begin(), root.end(), [](const auto& candidate) {
        return candidate.path == "node_modules/";
    }));

    const auto src = ursa::attachment_candidates(tmp.path, "src/ma");
    REQUIRE(src.size() == 1);
    CHECK(src[0].path == "src/main.cpp");
}

TEST_CASE("text attachment is snapshotted and encoded into the message")
{
    TempDir tmp;
    tmp.write("src/main.cpp", "old body\n");
    auto result = ursa::load_attachment(tmp.path, "src/main.cpp");
    REQUIRE(result.attachment);
    tmp.write("src/main.cpp", "new body\n");

    const std::string message
        = ursa::message_with_attachments("review it", { *result.attachment });
    CHECK(message.find("<file path=\"src/main.cpp\">") != std::string::npos);
    CHECK(message.find("old body") != std::string::npos);
    CHECK(message.find("new body") == std::string::npos);
}

TEST_CASE("attachments outside the workspace and binary files are rejected")
{
    TempDir tmp;
    tmp.write("binary.dat", std::string("a\0b", 3));
    CHECK_FALSE(ursa::load_attachment(tmp.path, "../outside.txt").attachment);
    CHECK_FALSE(ursa::load_attachment(tmp.path, "binary.dat").attachment);
}

TEST_CASE("session history keeps queued attachment snapshots")
{
    ursa::Session session;
    session.enqueue_message(
        "review", { { "src/main.cpp", "snapshot\n" } });
    auto queued = session.pop_queued();
    REQUIRE(queued);
    session.begin_send(
        std::move(queued->text), std::move(queued->attachments));

    const auto history = session.build_history("system");
    REQUIRE(history.size() == 2);
    CHECK(history.back().content.find("snapshot") != std::string::npos);
    CHECK(history.back().content.find("src/main.cpp") != std::string::npos);
}

TEST_CASE("removing a selected mention detaches its snapshot")
{
    std::vector<ursa::FileAttachment> attachments {
        { "src/main.cpp", "main" }, { "docs/design notes.md", "notes" }
    };
    ursa::retain_mentioned_attachments(
        "review @docs/design notes.md", attachments);

    REQUIRE(attachments.size() == 1);
    CHECK(attachments[0].path == "docs/design notes.md");

    ursa::retain_mentioned_attachments("review it", attachments);
    CHECK(attachments.empty());
}
