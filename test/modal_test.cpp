#include <doctest/doctest.h>
#include <json/json.h>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/screen.hpp>

#include "app/flows.h"
#include "common/util.h"
#include "conversation/format.h"
#include "network/json_io.h"
#include "permissions/filesystem.h"
#include "permissions/store.h"
#include "tools/skills.h"
#include "tools/tool.h"
#include "turn/delegation.h"
#include "ui/ui.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class PostPump {
public:
    imza::PostFn fn()
    {
        return [this](std::function<void()> f) { _push(std::move(f)); };
    }

    void pump()
    {
        for (;;) {
            std::function<void()> f;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    return;
                }
                f = std::move(queue_.front());
                queue_.pop_front();
            }
            f();
        }
    }

    bool wait_for(std::function<bool()> pred)
    {
        for (int i = 0; i < 10000; ++i) {
            pump();
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

private:
    void _push(std::function<void()> f)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(f));
    }

    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

imza::Config test_config()
{
    imza::Config cfg;
    imza::Connection conn;
    conn.id = "test";
    cfg.providers.push_back(conn);
    cfg.last_used = imza::LastUsed { "test", "m" };
    return cfg;
}

struct Env {
    PostPump pump;
    std::vector<imza::ChatRequest> requests;
    imza::StreamFn stream;
    std::shared_ptr<imza::ApplicationState> state;
    std::shared_ptr<imza::Session> session;

    explicit Env(imza::RuntimeFlag flags = imza::interactive_runtime_flags())
    {
        // Wire the lua tool the way application_state does, so the
        // bindings gate through the live permission store and the modal
        // queue instead of running in trusted mode.
        imza::LuaHost lua_host;
        lua_host.web_enabled
            = (flags & imza::RuntimeFlag::WEB) != imza::RuntimeFlag::NONE;
        lua_host.shell_enabled
            = (flags & imza::RuntimeFlag::SHELL) != imza::RuntimeFlag::NONE;
        lua_host.skip_permissions
            = (flags & imza::SKIP_PERMISSIONS) != imza::RuntimeFlag::NONE;
        lua_host.unattended
            = (flags & imza::RuntimeFlag::ATTENDED) == imza::RuntimeFlag::NONE;
        lua_host.context = [this] {
            return imza::permission_context(
                *state->environment, *state->permissions, session->mode());
        };
        lua_host.ask =
            [this, flags](
                imza::ModalPayload payload) -> std::future<imza::ModalResult> {
            if ((flags & imza::RuntimeFlag::ATTENDED)
                == imza::RuntimeFlag::NONE) {
                std::promise<imza::ModalResult> denied;
                denied.set_value(imza::ToolVerdict {
                    imza::ToolDecision::REJECT, "unattended run" });
                return denied.get_future();
            }
            return imza::request_modal(*state, std::move(payload));
        };
        lua_host.install_grants = [this](imza::PermissionStore::Grants grants) {
            return state->permissions->install(std::move(grants));
        };

        std::vector<imza::Tool> tools;
        tools.push_back(imza::make_skill_tool());
        tools.push_back(imza::make_subagent_tool());
        tools.push_back(imza::make_lua_tool(std::move(lua_host)));
        state = imza::make_application_state_with_tools(
            pump.fn(), test_config(), std::move(tools),
            [this](const imza::ChatRequest& req,
                const imza::StreamCallback& cb) { return stream(req, cb); },
            flags);
        session = state->session;
        REQUIRE(pump.wait_for([&] { return state->environment->ready(); }));
    }

    const imza::ChatRequest& last_request() const { return requests.back(); }

    size_t user_turn_count() const
    {
        size_t n = 0;
        for (const auto& it : session->items()) {
            if (std::holds_alternative<imza::UserTurn>(it)) {
                ++n;
            }
        }
        return n;
    }

    const imza::ToolCall* pending_tool() const
    {
        for (auto it = session->items().rbegin(); it != session->items().rend();
            ++it) {
            if (const auto* tc = std::get_if<imza::ToolCall>(&*it)) {
                return tc;
            }
        }
        return nullptr;
    }
};

bool showing_tool_ask(const imza::Session& st)
{
    return std::holds_alternative<imza::ToolCallRequest>(st.modal())
        && st.phase() == imza::Session::Phase::AWAITING;
}

bool showing_question(const imza::Session& st)
{
    return std::holds_alternative<imza::QuestionForm>(st.modal())
        && st.phase() == imza::Session::Phase::AWAITING;
}

bool idle(const imza::Session& st)
{
    return st.phase() == imza::Session::Phase::IDLE && st.modal().index() == 0;
}

} // namespace

TEST_CASE("plan requests omit edit and write tools")
{
    Env env;
    env.stream
        = [&env](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
              env.requests.push_back(req);
              cb(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "inspect");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    REQUIRE_FALSE(env.requests.empty());
    const auto& tools = env.requests.front().tools;
    CHECK(std::none_of(tools.begin(), tools.end(),
        [](const imza::ToolSpec& tool) { return tool.name == "edit"; }));
    CHECK(std::none_of(tools.begin(), tools.end(),
        [](const imza::ToolSpec& tool) { return tool.name == "write"; }));
    CHECK(std::any_of(tools.begin(), tools.end(),
        [](const imza::ToolSpec& tool) { return tool.name == "lua"; }));
}

TEST_CASE("attended root turn notifies once after completion")
{
    Env env;
    std::vector<imza::AgentNotification> notifications;
    env.state->notify_user = [&notifications](imza::AgentNotification event) {
        notifications.push_back(event);
    };
    env.stream
        = [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
              callback(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "inspect");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    REQUIRE(notifications.size() == 1);
    CHECK(notifications.front() == imza::AgentNotification::TURN_FINISHED);

    imza::on_turn_finished(*env.state, "");
    CHECK(notifications.size() == 1);
}

TEST_CASE("agent question notifies when input is required")
{
    Env env;
    std::vector<imza::AgentNotification> notifications;
    env.state->notify_user = [&notifications](imza::AgentNotification event) {
        notifications.push_back(event);
    };
    auto round = std::make_shared<int>(0);
    env.stream = [round](const imza::ChatRequest&,
                     const imza::StreamCallback& callback) {
        if ((*round)++ == 0) {
            callback(imza::make_question_event(
                { { "Continue?", { "yes" }, false, false } }));
        }
        callback(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "inspect");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));
    REQUIRE(notifications.size() == 1);
    CHECK(notifications.front() == imza::AgentNotification::INPUT_REQUIRED);

    imza::resolve_modal(*env.state,
        imza::ModalResult { imza::ModalAnswer { { { { "yes" }, "", "" } } } });
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    REQUIRE(notifications.size() == 2);
    CHECK(notifications.back() == imza::AgentNotification::TURN_FINISHED);
}

TEST_CASE("agent shell approval notifies when input is required")
{
    Env env;
    std::vector<imza::AgentNotification> notifications;
    env.state->notify_user = [&notifications](imza::AgentNotification event) {
        notifications.push_back(event);
    };
    env.stream = [](const imza::ChatRequest& request,
                     const imza::StreamCallback& callback) {
        if (request.messages.back().type == imza::Message::Type::USER) {
            callback(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('custom notify') print(code)"})json",
                "", "call" }));
        }
        callback(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "run it");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    REQUIRE(notifications.size() == 1);
    CHECK(notifications.front() == imza::AgentNotification::INPUT_REQUIRED);

    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::REJECT, "" });
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    REQUIRE(notifications.size() == 2);
    CHECK(notifications.back() == imza::AgentNotification::TURN_FINISHED);
}

TEST_CASE("user modal does not send an agent notification")
{
    Env env;
    std::vector<imza::AgentNotification> notifications;
    env.state->notify_user = [&notifications](imza::AgentNotification event) {
        notifications.push_back(event);
    };

    imza::enqueue_user_modal(
        *env.state, imza::ViewerModal { "Document", "content" });

    CHECK(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(notifications.empty());
    imza::close_modal(*env.state);
}

TEST_CASE("queued agent modal notifies only when presented")
{
    Env env;
    std::vector<imza::AgentNotification> notifications;
    env.state->notify_user = [&notifications](imza::AgentNotification event) {
        notifications.push_back(event);
    };
    imza::enqueue_user_modal(
        *env.state, imza::ViewerModal { "First", "content" });
    auto result = imza::request_modal(
        *env.state, imza::ViewerModal { "Agent", "content" });
    env.pump.pump();

    CHECK(notifications.empty());
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title == "First");

    imza::close_modal(*env.state);
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title == "Agent");
    REQUIRE(notifications.size() == 1);
    CHECK(notifications.front() == imza::AgentNotification::INPUT_REQUIRED);

    imza::close_modal(*env.state);
    CHECK(result.get().index() == 0);
}

TEST_CASE("plan mode rejects mutating file operations at the gate")
{
    Env env;
    const auto stamp
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory
        = std::filesystem::temp_directory_path()
        / ("imza-plan-write-" + std::to_string(stamp));
    std::filesystem::create_directories(directory);
    auto round = std::make_shared<int>(0);
    env.stream = [directory, round](
                     const imza::ChatRequest&, const imza::StreamCallback& cb) {
        if ((*round)++ == 0) {
            Json::Value arguments(Json::objectValue);
            arguments["script"] = "local ok, err = tool.file.write([["
                + (directory / "out.txt").string()
                + "]], 'no')\nprint(ok, err)";
            cb(imza::make_tool_call_event(
                { "lua", imza::write_json(arguments), "", "lua-call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "write");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    // The roster-level gate accepts a lua call, so the Plan-mode refusal
    // surfaces as a binding error inside the tool result.
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(call->result->text.find("file.write: permission denied")
        != std::string::npos);
    CHECK_FALSE(std::filesystem::is_regular_file(directory / "out.txt"));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST_CASE("subagent tool waits for a research agent and retains its chat")
{
    Env env;
    env.stream
        = [&env](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
              env.requests.push_back(req);
              const std::string last = req.messages.empty()
                  ? std::string { }
                  : req.messages.back().content;
              if (last.starts_with("delegate")) {
                  cb(imza::make_tool_call_event({ "subagent",
                      R"({"tasks":[{"mode":"research","prompt":"inspect"}]})",
                      "delegate inspection", "delegate-1" }));
              } else if (last.starts_with("inspect")) {
                  cb(imza::make_delta_event("research report"));
              } else {
                  cb(imza::make_delta_event("main complete"));
              }
              cb(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "delegate");
    const bool finished = env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; });
    CAPTURE(env.requests.size());
    if (!env.requests.empty() && !env.requests.front().messages.empty()) {
        CAPTURE(env.requests.front().messages.back().content);
    }
    CAPTURE(env.session->items().size());
    CAPTURE(static_cast<int>(env.session->phase()));
    CAPTURE(env.session->error());
    REQUIRE(finished);
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(call->result->text.find("research report") != std::string::npos);
    REQUIRE(call->subagent_chats.size() == 1);
    CHECK(call->subagent_chats[0].transcript.find("inspect")
        != std::string::npos);
    CHECK(call->subagent_chats[0].transcript.find("research report")
        != std::string::npos);
    const auto child_request = std::find_if(env.requests.begin(),
        env.requests.end(), [](const imza::ChatRequest& request) {
            return !request.messages.empty()
                && request.messages.back().content.starts_with("inspect");
        });
    REQUIRE(child_request != env.requests.end());
    REQUIRE(child_request->messages.size() >= 2);
    CHECK(child_request->messages.front().type == imza::Message::Type::SYSTEM);
    CHECK_FALSE(child_request->messages.front().content.empty());
    CHECK(child_request->messages.back().content == "inspect");
    CHECK(std::none_of(child_request->tools.begin(), child_request->tools.end(),
        [](const imza::ToolSpec& tool) {
            return tool.name == "subagent" || tool.name == "todo";
        }));
}

TEST_CASE("subagent tool captures two concurrent agents separately")
{
    Env env;
    env.stream = [](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        const std::string last = req.messages.empty()
            ? std::string { }
            : req.messages.back().content;
        if (last.starts_with("delegate two")) {
            cb(imza::make_tool_call_event({ "subagent",
                R"({"tasks":[{"mode":"research","prompt":"alpha"},{"mode":"research","prompt":"beta"}]})",
                "delegate two checks", "delegate-2" }));
        } else if (last.starts_with("alpha")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            cb(imza::make_delta_event("alpha report"));
        } else if (last.starts_with("beta")) {
            cb(imza::make_delta_event("beta report"));
        } else {
            cb(imza::make_delta_event("main joined reports"));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "delegate two");
    REQUIRE(env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; }));
    const imza::ToolCall& call = *env.pending_tool();
    REQUIRE(call.result.has_value());
    CHECK(call.result->text.find("alpha report") != std::string::npos);
    CHECK(call.result->text.find("beta report") != std::string::npos);
    REQUIRE(call.subagent_chats.size() == 2);
    CHECK(call.subagent_chats[0].transcript.find("alpha report")
        != std::string::npos);
    CHECK(call.subagent_chats[0].transcript.find("beta report")
        == std::string::npos);
    CHECK(call.subagent_chats[1].transcript.find("beta report")
        != std::string::npos);
    CHECK(call.subagent_chats[1].transcript.find("alpha report")
        == std::string::npos);
    const imza::SubagentChat first
        = env.state->delegation->subagent_chat(call, 0);
    const imza::SubagentChat second
        = env.state->delegation->subagent_chat(call, 1);
    CHECK(first.title == "Agent 1 (research)");
    CHECK(second.title == "Agent 2 (research)");
    CHECK(first.transcript != second.transcript);

    auto chat        = imza::make_chat(env.state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100 }; });
    auto click_agent = [&](std::string_view label) {
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(50));
        ftxui::Render(screen, chat->Render());
        const std::vector<std::string> lines
            = imza::split_lines(screen.ToString());
        for (std::size_t y = 0; y < lines.size(); ++y) {
            const std::size_t x = lines[y].find(label);
            if (x == std::string::npos)
                continue;
            ftxui::Mouse mouse;
            mouse.button = ftxui::Mouse::Left;
            mouse.motion = ftxui::Mouse::Pressed;
            mouse.x      = static_cast<int>(x);
            mouse.y      = static_cast<int>(y);
            return chat->OnEvent(ftxui::Event::Mouse("", mouse));
        }
        return false;
    };

    REQUIRE(click_agent("View Agent 1"));
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title
        == "Agent 1 (research)");
    imza::close_modal(*env.state);
    REQUIRE(click_agent("View Agent 2"));
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title
        == "Agent 2 (research)");
    imza::close_modal(*env.state);
}

TEST_CASE("delegated-agent approvals surface through the main modal queue")
{
    Env env;
    env.stream = [](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        const bool child = std::any_of(req.messages.begin(), req.messages.end(),
            [](const imza::Message& message) {
                return message.type == imza::Message::Type::USER
                    && message.content.starts_with("inspect");
            });
        const bool has_tool_result = std::any_of(req.messages.begin(),
            req.messages.end(), [](const imza::Message& message) {
                return message.type == imza::Message::Type::TOOL;
            });
        if (!child && !has_tool_result) {
            cb(imza::make_tool_call_event({ "subagent",
                R"({"tasks":[{"mode":"research","prompt":"inspect"}]})",
                "delegate inspection", "delegate-1" }));
        } else if (child && !has_tool_result) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local rows, err = tool.shell('probe child') print(err ~= nil)"})json",
                "probe child", "child-lua" }));
        } else {
            cb(imza::make_delta_event(
                child ? "child approved" : "main complete"));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "delegate");
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ToolCallRequest>(
            env.session->modal());
    }));
    const auto request = std::get<imza::ToolCallRequest>(env.session->modal());
    CHECK(request.description.find("Agent 1 (research)") != std::string::npos);
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    REQUIRE(env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->text.find("child approved") != std::string::npos);
}

TEST_CASE("subagent failure reports preserve the last completed tool output")
{
    Env env;
    env.stream = [](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        const bool child = std::any_of(req.messages.begin(), req.messages.end(),
            [](const imza::Message& message) {
                return message.type == imza::Message::Type::USER
                    && message.content.starts_with("failing child");
            });
        const bool has_tool_result = std::any_of(req.messages.begin(),
            req.messages.end(), [](const imza::Message& message) {
                return message.type == imza::Message::Type::TOOL;
            });
        if (!child && !has_tool_result) {
            cb(imza::make_tool_call_event({ "subagent",
                R"({"tasks":[{"mode":"research","prompt":"failing child"}]})",
                "delegate failing child", "delegate-failure" }));
        } else if (child && !has_tool_result) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('custom child') print('child-output')"})json",
                "run command", "child-lua" }));
        } else if (child) {
            cb(imza::make_error_event(
                imza::Status::API_ERROR, "follow-up failed"));
        } else {
            cb(imza::make_delta_event("main complete"));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "delegate failure");
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ToolCallRequest>(
            env.session->modal());
    }));
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    REQUIRE(env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; }));
    const imza::ToolCall& call = *env.pending_tool();
    REQUIRE(call.result.has_value());
    CHECK(call.result->text.find("Failed: API error") != std::string::npos);
    CHECK(call.result->text.find("Last completed tool output")
        != std::string::npos);
    CHECK(call.result->text.find("child-output") != std::string::npos);
    REQUIRE(call.subagent_chats.size() == 1);
    CHECK(call.subagent_chats[0].transcript.find("child-output")
        != std::string::npos);
}

TEST_CASE("subagent tool rejects build tasks while main agent is planning")
{
    Env env;
    env.stream
        = [](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
              if (!req.messages.empty()
                  && req.messages.back().content.starts_with("delegate")) {
                  cb(imza::make_tool_call_event({ "subagent",
                      R"({"tasks":[{"mode":"build","prompt":"change it"}]})",
                      "delegate change", "delegate-1" }));
              } else {
                  cb(imza::make_delta_event("main complete"));
              }
              cb(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "delegate");
    const bool finished = env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; });
    CAPTURE(env.session->items().size());
    CAPTURE(static_cast<int>(env.session->phase()));
    CAPTURE(env.session->error());
    REQUIRE(finished);
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::REJECT);
    CHECK(call->result->text.find("require main-agent build mode")
        != std::string::npos);
    CHECK(env.state->queue.size() == 0);
}

TEST_CASE(
    "question round-trip: AWAITING while pending, reply folded, ask stable")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_delta_event("I need input.\n"));
            cb(imza::make_question_event(
                { { "Which one?", { "A", "B" }, false, false } }));
        } else {
            cb(imza::make_delta_event("thanks"));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));
    CHECK(env.state->queue.size() == 1);

    const std::string ask_md = imza::question_form_markdown(
        { { "Which one?", { "A", "B" }, false, false } });
    auto assistant_corpus = [&] {
        std::string all;
        for (const auto& it : env.session->items()) {
            if (const auto* a = std::get_if<imza::AssistantTurn>(&it)) {
                all += a->markdown + "\n";
            }
        }
        return all;
    };
    const std::string snapshot = assistant_corpus();
    CHECK(snapshot.find(ask_md) != std::string::npos);

    imza::resolve_modal(*env.state,
        imza::ModalResult { imza::ModalAnswer { { { { "B" }, "", "" } } } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.state->queue.size() == 0);

    const std::string after = assistant_corpus();
    CHECK(after.find(ask_md) != std::string::npos);
    CHECK(after.find(ask_md) == after.rfind(ask_md));

    size_t answers = 0;
    for (const auto& it : env.session->items()) {
        if (std::holds_alternative<imza::ModalAnswer>(it)) {
            ++answers;
        }
    }
    REQUIRE(answers == 1);
    CHECK(env.user_turn_count() == 1);
    CHECK(env.last_request().messages.back().type == imza::Message::Type::USER);
    CHECK(env.last_request().messages.back().content.find("User answered:")
        != std::string::npos);
    CHECK(env.last_request().messages.back().content.find("> B")
        != std::string::npos);
}

TEST_CASE("tool accept: output fills result, request half byte-stable")
{
    Env env;
    env.session->set_mode(imza::Session::Mode::BUILD);
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('inspect -la') print(out) print('shell-complete')"})json",
                "list files" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    const imza::ToolCall* pending = env.pending_tool();
    REQUIRE(pending != nullptr);
    CHECK(pending->name == "lua");
    CHECK_FALSE(pending->result.has_value());

    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    const imza::ToolCall* done = env.pending_tool();
    REQUIRE(done != nullptr);
    REQUIRE(done->result.has_value());
    CHECK(done->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(done->name == "lua");

    const auto& msgs = env.last_request().messages;
    REQUIRE(msgs.size() >= 2);
    CHECK(msgs.back().type == imza::Message::Type::TOOL);
    CHECK(msgs.back().content.find("shell-complete") != std::string::npos);
    const auto& prev = msgs[msgs.size() - 2];
    CHECK(prev.type == imza::Message::Type::ASSISTANT);
    REQUIRE(prev.tool_calls.size() == 1);
    CHECK(prev.tool_calls[0].name == "lua");

    CHECK_FALSE(env.last_request().tools.empty());
    CHECK(env.user_turn_count() == 1);
}

TEST_CASE("reject with reason reaches transcript and injected result")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, err = tool.shell('rm -rf /') print(out, err)"})json",
                "danger" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    imza::resolve_modal(*env.state,
        imza::ModalResult { imza::ToolVerdict {
            imza::ToolDecision::REJECT, "needs approval first" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(tc->result->text.find("shell: needs approval first")
        != std::string::npos);

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find(
              "shell: needs approval first")
        != std::string::npos);
}

TEST_CASE("esc on tool injects generic denial, appends nothing to transcript")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, err = tool.shell('inspect') print(out, err)"})json",
                "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    imza::close_modal(*env.state);

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(tc->result->text.find("shell: permission dismissed")
        != std::string::npos);
    CHECK(env.user_turn_count() == 1);

    REQUIRE(env.requests.size() == 1);
    CHECK(env.state->queue.size() == 0);
}

TEST_CASE("esc on question skips form, appends nothing, no exception")
{
    Env env;
    env.stream
        = [&env](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
              env.requests.push_back(req);
              cb(imza::make_question_event(
                  { { "Pick", { "x", "y" }, false, false } }));
              cb(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));

    imza::close_modal(*env.state);

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.user_turn_count() == 1);
    size_t answers = 0;
    for (const auto& it : env.session->items()) {
        if (std::holds_alternative<imza::ModalAnswer>(it)) {
            ++answers;
        }
    }
    CHECK(answers == 0);
}

TEST_CASE("one drain cycle folds question answer and tool output correctly")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_question_event(
                { { "Backend?", { "pg", "sqlite" }, false, false } }));
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('whoami') print(out) print('whoami-complete')"})json",
                "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");

    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));
    CHECK(env.state->queue.size() == 1);
    imza::resolve_modal(*env.state,
        imza::ModalResult { imza::ModalAnswer { { { { "pg" }, "", "" } } } });

    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    CHECK(env.user_turn_count() == 1);
    const auto& msgs = env.last_request().messages;
    int reply_idx    = -1;
    int tool_idx     = -1;
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].content.find("User answered:") != std::string::npos) {
            reply_idx = static_cast<int>(i);
        }
        if (msgs[i].content.find("whoami-complete") != std::string::npos
            && msgs[i].type == imza::Message::Type::TOOL) {
            tool_idx = static_cast<int>(i);
        }
    }
    REQUIRE(reply_idx >= 0);
    REQUIRE(tool_idx >= 0);
    CHECK(tool_idx < reply_idx);
    CHECK(msgs[tool_idx].type == imza::Message::Type::TOOL);
    const auto& prev = msgs[tool_idx - 1];
    CHECK(prev.type == imza::Message::Type::ASSISTANT);
    REQUIRE(prev.tool_calls.size() == 1);
    CHECK(prev.tool_calls[0].name == "lua");
    CHECK(prev.tool_calls[0].args.find("whoami-complete") != std::string::npos);
}

TEST_CASE("FIFO order preserved and queue_size counts overlays")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_question_event({ { "Q1", { "a" }, false, false } }));
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('cmake --build build') print(code)"})json",
                "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));

    imza::enqueue_user_modal(
        *env.state, imza::ViewerModal { "Queued", "content" });
    env.pump.pump();
    CHECK(env.state->queue.size() == 2);

    imza::resolve_modal(*env.state,
        imza::ModalResult { imza::ModalAnswer { { { { "a" }, "", "" } } } });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ViewerModal>(env.session->modal())
            && env.state->queue.size() == 2;
    }));
    CHECK(env.state->queue.size() == 2);

    imza::close_modal(*env.state);
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    CHECK(env.state->queue.size() == 1);

    imza::close_modal(*env.state);
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.state->queue.size() == 0);
}

TEST_CASE("markdown viewer renders markdown instead of source lines")
{
    Env env;
    imza::enqueue_user_modal(*env.state,
        imza::ViewerModal { "Document", "# Heading\n\n```cpp\nreturn 1;\n```",
            "markdown", 1, true, "" });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ViewerModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    auto screen            = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(12));
    ftxui::Render(screen, modal->Render());
    const std::string rendered = screen.ToString();
    CHECK(rendered.find("Heading") != std::string::npos);
    CHECK(rendered.find("# Heading") == std::string::npos);
}

TEST_CASE("one-time shell approval does not authorize later calls")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        switch ((*round)++) {
        case 0:
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('custom one') print(code)"})json",
                "" }));
            break;
        case 1:
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('custom two') print(code)"})json",
                "" }));
            break;
        default: break;
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" } });

    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    CHECK(env.state->queue.size() == 1);
    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::REJECT, "" } });
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    std::vector<imza::ToolCall::Result> results;
    for (const auto& it : env.session->items()) {
        if (const auto* tc = std::get_if<imza::ToolCall>(&it)) {
            REQUIRE(tc->result.has_value());
            results.push_back(*tc->result);
        }
    }
    REQUIRE(results.size() == 2);
    CHECK(results[0].kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(results[1].kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(results[1].text.find("shell: rejected") != std::string::npos);
    CHECK(env.user_turn_count() == 1);
}

TEST_CASE("filesystem session approval installs an exact reusable grant")
{
    Env env;
    env.session->set_mode(imza::Session::Mode::BUILD);
    const auto stamp
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory
        = std::filesystem::temp_directory_path()
        / ("imza-phase5-modal-" + std::to_string(stamp));
    const std::filesystem::path path = directory / "approved.txt";
    std::filesystem::create_directories(directory);
    auto round = std::make_shared<int>(0);
    env.stream = [path, round](
                     const imza::ChatRequest&, const imza::StreamCallback& cb) {
        if ((*round)++ < 2) {
            Json::Value arguments(Json::objectValue);
            arguments["script"] = "assert(tool.file.write([[" + path.string()
                + "]], 'approved'))";
            cb(imza::make_tool_call_event(
                { "lua", imza::write_json(arguments), "", "lua-call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    const auto request = std::get<imza::ToolCallRequest>(env.session->modal());
    CHECK(request.name == "write");
    CHECK(request.allow_for_session);
    imza::resolve_modal(*env.state,
        imza::ToolVerdict { imza::ToolDecision::ACCEPT_FOR_SESSION, "" });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.state->queue.size() == 0);
    CHECK(env.state->permissions->snapshot()->size() == 1);
    for (const auto& item : env.session->items()) {
        if (const auto* call = std::get_if<imza::ToolCall>(&item);
            call != nullptr && call->name == "lua" && call->result) {
            CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
        }
    }
    CHECK(std::filesystem::is_regular_file(path));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST_CASE("dangerous skip accepts permission-gated tools without a modal")
{
    Env env(static_cast<imza::RuntimeFlag>(
        imza::ATTENDED | imza::SHELL | imza::SKIP_PERMISSIONS));
    env.stream = [](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        if (req.messages.back().type == imza::Message::Type::USER) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('custom skipped') print(code)"})json",
                "", "call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(env.state->queue.size() == 0);
    CHECK_FALSE(env.state->runner->blocked_permission());
}

TEST_CASE("dangerous skip does not weaken hard rejection")
{
    Env env(
        static_cast<imza::RuntimeFlag>(imza::SHELL | imza::SKIP_PERMISSIONS));
    env.stream = [](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        if (req.messages.back().type == imza::Message::Type::USER) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, err = tool.shell('') print(err)"})json",
                "", "call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.state->queue.size() == 0);
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(call->result->text.find("empty command") != std::string::npos);
}

TEST_CASE("tools with an automatic policy run without an approval modal")
{
    Env env(static_cast<imza::RuntimeFlag>(
        imza::interactive_runtime_flags() | imza::RuntimeFlag::WEB));
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local body, err = tool.web.search('imza') if err then error(err) end print('searched: imza')"})json",
                "", "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    CHECK(env.state->queue.size() == 0);

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(tc->result->text.find("searched: imza") != std::string::npos);

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find("searched: imza")
        != std::string::npos);
}

TEST_CASE("unknown tools error back to the model without a modal")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "nope", "{}", "", "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    CHECK(env.state->queue.size() == 0);

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::ERROR);
    CHECK(tc->result->text.find("unknown tool: nope") != std::string::npos);

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find("unknown tool: nope")
        != std::string::npos);
}

// Display text of a rendered row: ANSI-styled cells decode to their visible
// characters, so byte offsets become click columns.
std::string plain_row(const std::string& row)
{
    std::string out;
    for (std::size_t i = 0; i < row.size();) {
        if (row[i] == '\x1b') {
            ++i;
            if (i < row.size() && row[i] == '[') {
                ++i;
                while (i < row.size()
                    && !std::isalpha(static_cast<unsigned char>(row[i]))) {
                    ++i;
                }
                ++i;
            }
            continue;
        }
        out += row[i++];
    }
    return out;
}

TEST_CASE("modal action buttons respond to mouse clicks")
{
    Env env;
    env.session->set_mode(imza::Session::Mode::BUILD);
    env.stream = [&env](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if (req.messages.back().type == imza::Message::Type::USER) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = tool.shell('custom click') print(code)"})json",
                "", "call-1" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    ftxui::Component modal = imza::make_modal(env.state);
    modal->Render();
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen, modal->Render());
    const std::vector<std::string> lines = imza::split_lines(screen.ToString());
    bool clicked                         = false;
    for (std::size_t y = 0; y < lines.size() && !clicked; ++y) {
        const std::size_t x = plain_row(lines[y]).find("Allow once");
        if (x == std::string::npos) {
            continue;
        }
        ftxui::Mouse mouse;
        mouse.button = ftxui::Mouse::Left;
        mouse.motion = ftxui::Mouse::Pressed;
        mouse.x      = static_cast<int>(x);
        mouse.y      = static_cast<int>(y);
        clicked      = modal->OnEvent(ftxui::Event::Mouse("", mouse));
    }
    CHECK(clicked);
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
}
