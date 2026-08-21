#pragma once

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <optional>
#include <thread>
#include <string>
#include <variant>
#include <vector>

#include "network.hpp"
#include "types.hpp"

namespace ursa {

struct LayoutCtx {
    enum class Kind { WIDE, NARROW };
    Kind kind;
    int width;
};

struct UserTurn {
    std::string text;
};

struct AssistantTurn {
    std::string markdown;
};

struct ToolCall {
    std::string name;
    std::string args;
    Status state = Status::OK;
    std::string result;
};

struct TodoItem {
    std::string text;
    bool done = false;
};

struct TodoList {
    std::vector<TodoItem> items;
};

struct ChangedFile {
    std::string path;
    std::string status;
};

struct Question {
    std::string prompt;
    std::vector<std::string> options;
};

using ConversationItem
    = std::variant<UserTurn, AssistantTurn, ToolCall, TodoList, Question>;

struct SettingsModal {
    std::string model;
};

using Modal = std::variant<std::monostate, SettingsModal, Question>;

struct UiState {
    enum class Phase { IDLE, STREAMING };
    std::vector<ConversationItem> items;
    Modal modal = std::monostate {};
    Phase phase = Phase::IDLE;
    std::string error;

    TodoList todo;
    std::vector<ChangedFile> changed_files;
    std::optional<Question> question;
};

using PostFn = std::function<void(std::function<void()>)>;

class Controller {
public:
    Controller(const Config& cfg, PostFn post);

    void submit(std::string text);
    UiState& state() { return state_; }
    const UiState& state() const { return state_; }
    const Config& config() const { return cfg_; }

private:
    void apply(const StreamEvent& ev);
    void finish(std::string error);

    Config cfg_;
    UiState state_;
    PostFn post_;
    std::optional<std::jthread> worker_;
};

std::string error_text(Status st);

int run_repl(const Config& cfg);

ftxui::Component make_chat(
    Controller& controller, std::function<void()> on_exit, std::function<int()> width);
ftxui::Component make_todo(Controller& controller, std::function<int()> width);
ftxui::Component make_changed_files(Controller& controller);
ftxui::Component make_settings(Controller& controller);
ftxui::Component make_question(Controller& controller);
ftxui::Component make_toolcall(Controller& controller);

} // namespace ursa
