#include <doctest/doctest.h>
#include <ftxui/component/event.hpp>

#include "app/application_state.h"
#include "conversation/input_history.h"
#include "ui/ui.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct HistoryFile {
    std::filesystem::path directory
        = std::filesystem::temp_directory_path() / "imza_input_history_test";
    std::filesystem::path path = directory / "history.json";

    HistoryFile()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    ~HistoryFile()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

ftxui::Component make_test_chat(
    const std::shared_ptr<imza::ApplicationState>& state)
{
    return imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
}

void type(ftxui::Component& chat, std::string_view text)
{
    for (const char character : text) {
        chat->OnEvent(ftxui::Event::Character(character));
    }
}

ftxui::Event ctrl_up() { return ftxui::Event::ArrowUpCtrl; }

ftxui::Event ctrl_down() { return ftxui::Event::ArrowDownCtrl; }

} // namespace

TEST_CASE(
    "input history persists a bounded list without consecutive duplicates")
{
    HistoryFile file;
    imza::InputHistoryStore history(file.path, 3);
    REQUIRE(history.record("one") == imza::Status::OK);
    REQUIRE(history.record("one") == imza::Status::OK);
    REQUIRE(history.record("two") == imza::Status::OK);
    REQUIRE(history.record("three") == imza::Status::OK);
    REQUIRE(history.record("four") == imza::Status::OK);

    CHECK(history.entries()
        == std::vector<std::string> { "two", "three", "four" });
    const imza::InputHistoryStore reloaded(file.path, 3);
    CHECK(reloaded.entries()
        == std::vector<std::string> { "two", "three", "four" });

    imza::InputHistoryStore concurrent(file.path, 3);
    REQUIRE(concurrent.record("five") == imza::Status::OK);
    REQUIRE(history.record("six") == imza::Status::OK);
    const imza::InputHistoryStore merged(file.path, 3);
    CHECK(
        merged.entries() == std::vector<std::string> { "four", "five", "six" });

    imza::InputHistoryStore bounded(file.path);
    for (int index = 0; index <= 100; ++index) {
        REQUIRE(bounded.record("entry " + std::to_string(index))
            == imza::Status::OK);
    }
    const std::vector<std::string> entries = bounded.entries();
    REQUIRE(entries.size() == 100);
    CHECK(entries.front() == "entry 1");
    CHECK(entries.back() == "entry 100");
}

TEST_CASE("input history recovers from malformed storage")
{
    HistoryFile file;
    std::filesystem::create_directories(file.directory);
    std::ofstream(file.path) << "not json";

    imza::InputHistoryStore history(file.path);
    CHECK(history.entries().empty());
    REQUIRE(history.record("replacement") == imza::Status::OK);
    CHECK(history.entries() == std::vector<std::string> { "replacement" });
}

TEST_CASE("chat input history recalls entries and restores the draft")
{
    HistoryFile file;
    auto state = imza::make_application_state(
        [](std::function<void()> fn) { fn(); }, imza::Config { });
    state->input_history = std::make_shared<imza::InputHistoryStore>(file.path);
    REQUIRE(state->input_history->record("first") == imza::Status::OK);
    REQUIRE(
        state->input_history->record("multiple\nlines") == imza::Status::OK);
    REQUIRE(state->input_history->record("second") == imza::Status::OK);
    auto chat = make_test_chat(state);

    type(chat, "draft");
    REQUIRE(chat->OnEvent(ctrl_up()));
    chat->OnEvent(ftxui::Event::Return);
    CHECK(state->input_history->entries().back() == "second");

    type(chat, "draft");
    REQUIRE(chat->OnEvent(ctrl_up()));
    REQUIRE(chat->OnEvent(ctrl_up()));
    REQUIRE(chat->OnEvent(ctrl_up()));
    REQUIRE(chat->OnEvent(ctrl_down()));
    REQUIRE(chat->OnEvent(ctrl_down()));
    REQUIRE(chat->OnEvent(ctrl_down()));
    chat->OnEvent(ftxui::Event::Return);
    CHECK(state->input_history->entries().back() == "draft");
}

TEST_CASE("plain arrows do not recall chat input history")
{
    HistoryFile file;
    auto state = imza::make_application_state(
        [](std::function<void()> fn) { fn(); }, imza::Config { });
    state->input_history = std::make_shared<imza::InputHistoryStore>(file.path);
    REQUIRE(state->input_history->record("previous") == imza::Status::OK);
    auto chat = make_test_chat(state);

    type(chat, "draft");
    REQUIRE(chat->OnEvent(ftxui::Event::ArrowUp));
    REQUIRE(chat->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(chat->OnEvent(ftxui::Event::Return));

    CHECK(state->input_history->entries().back() == "draft");
}

TEST_CASE("chat records slash commands and queued prompts")
{
    HistoryFile file;
    auto state = imza::make_application_state(
        [](std::function<void()> fn) { fn(); }, imza::Config { });
    state->input_history = std::make_shared<imza::InputHistoryStore>(file.path);
    auto chat            = make_test_chat(state);

    type(chat, "/new");
    REQUIRE(chat->OnEvent(ftxui::Event::Return));
    state->session->set_phase(imza::Session::Phase::STREAMING);
    type(chat, "queued prompt");
    REQUIRE(chat->OnEvent(ftxui::Event::Return));

    CHECK(state->input_history->entries()
        == std::vector<std::string> { "/new", "queued prompt" });
    REQUIRE(state->session->queued().size() == 1);
    CHECK(state->session->queued().front().text == "queued prompt");
}
TEST_CASE("bracketed multiline paste is submitted as one history entry")
{
    HistoryFile file;
    auto state = imza::make_application_state(
        [](std::function<void()> fn) { fn(); }, imza::Config { });
    state->input_history = std::make_shared<imza::InputHistoryStore>(file.path);
    auto chat            = make_test_chat(state);

    REQUIRE(chat->OnEvent(ftxui::Event::Special("\x1B[200~")));
    type(chat, "  line one");
    REQUIRE(chat->OnEvent(ftxui::Event::Return));
    type(chat, "line two  ");
    REQUIRE(chat->OnEvent(ftxui::Event::Special("\x1B[201~")));
    REQUIRE(chat->OnEvent(ftxui::Event::Return));

    CHECK(state->input_history->entries()
        == std::vector<std::string> { "  line one\nline two  " });
}
