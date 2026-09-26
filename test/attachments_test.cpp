#include <algorithm>
#include <filesystem>
#include <fstream>
#include <tuple>

#include <doctest/doctest.h>

#include "conversation/session.h"
#include "test_helpers.h"
#include "workspace/attachments.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() / "imza_attachment_test";

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
    auto token = imza::attachment_token_at("review @src/ma", 14);
    REQUIRE(token);
    CHECK(token->query == "src/ma");
    CHECK_FALSE(imza::attachment_token_at("me@example.com", 14));
}

TEST_CASE("attachment candidates list one directory without large directories")
{
    TempDir tmp;
    tmp.write("src/main.cpp", "int main() {}\n");
    tmp.write("readme.md", "hello\n");

    const auto root = imza::attachment_candidates(tmp.path, "");
    CHECK(std::none_of(root.begin(), root.end(), [](const auto& candidate) {
        return candidate.path == "node_modules/";
    }));

    const auto src = imza::attachment_candidates(tmp.path, "src/ma");
    REQUIRE(src.size() == 1);
    CHECK(src[0].path == "src/main.cpp");
}

TEST_CASE("attachments are classified by signatures instead of extensions")
{
    TempDir tmp;
    const std::vector<std::tuple<std::string, std::string,
        imza::Attachment::Type, std::string>>
        cases {
            { "png.txt", std::string("\x89PNG\r\n\x1a\n", 8),
                imza::Attachment::Type::IMAGE, "image/png" },
            { "jpeg.data", std::string("\xff\xd8\xff", 3),
                imza::Attachment::Type::IMAGE, "image/jpeg" },
            { "gif.bin", "GIF89a", imza::Attachment::Type::IMAGE, "image/gif" },
            { "webp.unknown", "RIFF1234WEBP", imza::Attachment::Type::IMAGE,
                "image/webp" },
            { "document.dat", "%PDF-1.7\n", imza::Attachment::Type::PDF,
                "application/pdf" },
        };

    for (const auto& [path, content, type, media_type] : cases) {
        tmp.write(path, content);
        const auto loaded = imza::load_attachment(tmp.path, path);
        REQUIRE(loaded.attachment);
        CHECK(loaded.attachment->type == type);
        CHECK(loaded.attachment->media_type == media_type);
        CHECK(loaded.attachment->content == content);
    }

    tmp.write("text.png", "not actually an image");
    const auto text = imza::load_attachment(tmp.path, "text.png");
    REQUIRE(text.attachment);
    CHECK(text.attachment->type == imza::Attachment::Type::TEXT);
    CHECK(text.attachment->media_type.empty());
}

TEST_CASE("native attachments are not inserted into prompt text")
{
    const std::vector<imza::Attachment> attachments {
        { "image.png", "binary image", imza::Attachment::Type::IMAGE,
            "image/png" },
        { "notes.txt", "read me", imza::Attachment::Type::TEXT, "" },
        { "document.pdf", "binary pdf", imza::Attachment::Type::PDF,
            "application/pdf" },
    };

    const std::string message
        = imza::message_with_attachments("review", attachments);
    CHECK(message.find("notes.txt") != std::string::npos);
    CHECK(message.find("read me") != std::string::npos);
    CHECK(message.find("image.png") == std::string::npos);
    CHECK(message.find("binary image") == std::string::npos);
    CHECK(message.find("document.pdf") == std::string::npos);
    CHECK(imza::message_with_attachments(
              "review", { attachments.front(), attachments.back() })
        == "review");
}

TEST_CASE("session history attaches native media to user messages")
{
    imza::Session session;
    session.begin_send("review",
        { { "notes.txt", "read me" },
            { "photo.png", "binary image", imza::Attachment::Type::IMAGE,
                "image/png" },
            { "doc.pdf", "binary pdf", imza::Attachment::Type::PDF,
                "application/pdf" } });

    const auto history = session.build_history("system");
    REQUIRE(history.size() == 2);
    CHECK(history.back().content.find("read me") != std::string::npos);
    CHECK(history.back().content.find("binary image") == std::string::npos);
    CHECK(history.back().content.find("binary pdf") == std::string::npos);

    REQUIRE(history.back().media.size() == 2);
    CHECK(history.back().media[0].type == imza::Attachment::Type::IMAGE);
    CHECK(history.back().media[0].path == "photo.png");
    CHECK(history.back().media[0].media_type == "image/png");
    CHECK(history.back().media[0].content == "binary image");
    CHECK(history.back().media[1].type == imza::Attachment::Type::PDF);
    CHECK(history.back().media[1].path == "doc.pdf");
    CHECK(history.back().media[1].media_type == "application/pdf");
    CHECK(history.back().media[1].content == "binary pdf");
}

TEST_CASE("text attachment is snapshotted and encoded into the message")
{
    TempDir tmp;
    tmp.write("src/main.cpp", "old body\n");
    auto result = imza::load_attachment(tmp.path, "src/main.cpp");
    REQUIRE(result.attachment);
    tmp.write("src/main.cpp", "new body\n");

    const std::string message
        = imza::message_with_attachments("review it", { *result.attachment });
    CHECK(message.find("<file path=\"src/main.cpp\">") != std::string::npos);
    CHECK(message.find("old body") != std::string::npos);
    CHECK(message.find("new body") == std::string::npos);
}

TEST_CASE("attachments outside the workspace and binary files are rejected")
{
    TempDir tmp;
    tmp.write("binary.dat", std::string("a\0b", 3));
    CHECK_FALSE(imza::load_attachment(tmp.path, "../outside.txt").attachment);
    CHECK_FALSE(imza::load_attachment(tmp.path, "binary.dat").attachment);
}

TEST_CASE("byte limits apply to text but not native media")
{
    TempDir tmp;
    const std::string big_text(1024 * 1024 + 1, 'x');
    tmp.write("big.txt", big_text);
    CHECK_FALSE(imza::load_attachment(tmp.path, "big.txt").attachment);

    const std::string big_png
        = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(1200 * 1024, '\0');
    tmp.write("big.png", big_png);
    const auto image = imza::load_attachment(tmp.path, "big.png");
    REQUIRE(image.attachment);
    CHECK(image.attachment->type == imza::Attachment::Type::IMAGE);
    CHECK(image.attachment->content == big_png);

    const std::string big_pdf = "%PDF-1.7\n" + std::string(1200 * 1024, '\0');
    tmp.write("big.pdf", big_pdf);
    const auto pdf = imza::load_attachment(tmp.path, "big.pdf");
    REQUIRE(pdf.attachment);
    CHECK(pdf.attachment->type == imza::Attachment::Type::PDF);
    CHECK(pdf.attachment->content == big_pdf);

    std::string error;
    const std::vector<imza::Attachment> existing {
        { "notes.txt", std::string(3 * 1024 * 1024, 'x') },
    };
    const imza::Attachment big_media { "big.png", big_png,
        imza::Attachment::Type::IMAGE, "image/png" };
    CHECK(imza::can_add_attachment(existing, big_media, error));
    CHECK(imza::can_add_attachment(existing,
              { "more.txt", std::string(2 * 1024 * 1024, 'x') }, error)
        == false);
    CHECK(error.find("4 MiB") != std::string::npos);

    std::vector<imza::Attachment> full(20, big_media);
    CHECK(imza::can_add_attachment(full, big_media, error) == false);
}

TEST_CASE("session history keeps queued attachment snapshots")
{
    imza::Session session;
    session.enqueue_message("review", { { "src/main.cpp", "snapshot\n" } });
    auto queued = session.pop_queued();
    REQUIRE(queued);
    session.begin_send(std::move(queued->text), std::move(queued->attachments));

    const auto history = session.build_history("system");
    REQUIRE(history.size() == 2);
    CHECK(history.back().content.find("snapshot") != std::string::npos);
    CHECK(history.back().content.find("src/main.cpp") != std::string::npos);
}

TEST_CASE("session exposes unique attachment basenames and publishes changes")
{
    imza::Session session;
    int changes = 0;
    auto subscription
        = session.subscribe_to_attachments_change([&] { ++changes; });

    session.begin_send("review",
        { { "src/main.cpp", "one" }, { "docs/main.cpp", "two" },
            { "docs/design.md", "three" } });

    CHECK(session.attachment_names()
        == std::vector<std::string> { "main.cpp", "design.md" });
    CHECK(changes == 1);

    imza::SessionSnapshot snapshot;
    snapshot.items.push_back(
        imza::UserTurn { "restored", { { "notes/plan.txt", "content" } } });
    session.restore(std::move(snapshot));

    CHECK(
        session.attachment_names() == std::vector<std::string> { "plan.txt" });
    CHECK(changes == 2);
}

TEST_CASE("session compaction replaces only old model history")
{
    imza::Session session;
    session.begin_send("old request");
    session.append_assistant();
    session.apply(imza::make_delta_event("old answer"), { });
    session.begin_send("current request");
    session.append_assistant();

    const auto [id, prefix] = session.begin_compaction();
    session.finish_compaction(id, "preserved summary", prefix, true);

    CHECK(session.items().size() == 5);
    const auto history = session.build_history("system");
    REQUIRE(history.size() == 4);
    CHECK(history[1].content.find("preserved summary") != std::string::npos);
    CHECK(history[2].content == "current request");
    CHECK(history[3].type == imza::Message::Type::ASSISTANT);
}

TEST_CASE("session reports pending turns, queued messages and tools")
{
    imza::Session session;
    CHECK_FALSE(session.has_pending_work());

    session.begin_send("request");
    CHECK(session.has_pending_work());
    REQUIRE(session.finish_session(""));
    CHECK_FALSE(session.has_pending_work());

    session.enqueue_message("queued");
    CHECK(session.has_pending_work());
    REQUIRE(session.pop_queued());
    CHECK_FALSE(session.has_pending_work());

    imza::ToolCallRequest request;
    request.id   = "call-1";
    request.name = "shell";
    request.args = R"({"command":"sleep 2"})";
    imza::test::append_tool(session, request);
    CHECK(session.has_pending_work());
    session.fill_tool_result(
        request, { imza::ToolCall::Result::Kind::OUTPUT, "done" });
    CHECK_FALSE(session.has_pending_work());
}

TEST_CASE("streamed tool call starts planning then executes in place")
{
    imza::Session session;
    session.begin_send("request");
    session.append_assistant();

    imza::ToolCallRequest request;
    request.id   = "call-1";
    request.name = "lua";
    request.args = R"({"script":"return 1"})";

    // The start event creates a single planning item with no arguments yet.
    session.apply(imza::make_tool_call_start_event(request), { });
    REQUIRE(session.items().size() == 3);
    const auto* planning = std::get_if<imza::ToolCall>(&session.items()[2]);
    REQUIRE(planning != nullptr);
    CHECK(planning->phase == imza::ToolCall::Phase::PLANNING);
    CHECK(planning->args.empty());
    // A still-streaming call is not part of the model history.
    CHECK(session.build_history("system").size() == 3);

    // The complete call updates that same item rather than appending a new one.
    session.apply(imza::make_tool_call_event(request), { });
    REQUIRE(session.items().size() == 3);
    const auto* executing = std::get_if<imza::ToolCall>(&session.items()[2]);
    REQUIRE(executing != nullptr);
    CHECK(executing->phase == imza::ToolCall::Phase::EXECUTING);
    CHECK(executing->args == request.args);
    CHECK(executing->id == planning->id);

    // Now the call is part of history.
    const auto history = session.build_history("system");
    REQUIRE(history.size() == 4);
    CHECK(history[2].tool_calls.size() == 1);

    imza::Session interrupted;
    interrupted.begin_send("request");
    interrupted.append_assistant();
    interrupted.apply(imza::make_tool_call_start_event(request), { });
    REQUIRE(interrupted.finish_session(""));
    REQUIRE(interrupted.items().size() == 2);
    CHECK(std::holds_alternative<imza::AssistantTurn>(interrupted.items()[1]));
}

TEST_CASE("removing a selected mention detaches its snapshot")
{
    std::vector<imza::Attachment> attachments { { "src/main.cpp", "main" },
        { "docs/design notes.md", "notes" } };
    imza::retain_mentioned_attachments(
        "review @docs/design notes.md", attachments);

    REQUIRE(attachments.size() == 1);
    CHECK(attachments[0].path == "docs/design notes.md");

    imza::retain_mentioned_attachments("review it", attachments);
    CHECK(attachments.empty());
}
