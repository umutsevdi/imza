#pragma once

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "commands.h"
#include "network.h"
#include "types.h"

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

struct HelpModal { };

using Modal = std::variant<std::monostate, SettingsModal, HelpModal, Question>;

struct UiState {
    enum class Phase { IDLE, STREAMING };
    enum class Mode { PLAN, BUILD };
    std::vector<ConversationItem> items;
    Modal modal = std::monostate { };
    Phase phase = Phase::IDLE;
    Mode mode   = Mode::PLAN;
    std::string error;

    TodoList todo;
    std::vector<ChangedFile> changed_files;
    std::optional<Question> question;
};

using PostFn = std::function<void(std::function<void()>)>;

class Controller {
public:
    Controller(const Config& cfg, PostFn post, std::function<void()> on_exit);

    void submit(std::string text);
    void toggle_mode();
    void open_demo();
    void open_help();
    void set_error(std::string msg);
    void close_modal();
    UiState& state() { return state_; }
    const UiState& state() const { return state_; }
    const Config& config() const { return cfg_; }
    const std::vector<SlashCommand>& commands() const { return commands_; }

private:
    void submit_message(std::string text);
    void run_slash(std::string_view cmd);
    void apply(const StreamEvent& ev);
    void finish(std::string error);

    Config cfg_;
    UiState state_;
    PostFn post_;
    std::function<void()> on_exit_;
    std::vector<SlashCommand> commands_;
    std::optional<std::jthread> worker_;
};

std::string error_text(Status st);

int run_repl(const Config& cfg);

ftxui::Component make_chat(Controller& controller, std::function<int()> width);
ftxui::Component make_todo(Controller& controller, std::function<int()> width);
ftxui::Component make_changed_files(Controller& controller);
ftxui::Component make_settings(Controller& controller);
ftxui::Component make_question(Controller& controller);
ftxui::Component make_toolcall(Controller& controller);

} // namespace ursa
