#include <doctest/doctest.h>
#include <json/json.h>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/screen.hpp>

#include "format.h"
#include "review.h"
#include "tools.h"
#include "ui.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class PostPump {
public:
    ursa::PostFn fn()
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

ursa::Config test_config()
{
    ursa::Config cfg;
    ursa::Connection conn;
    conn.id          = "test";
    conn.provider_id = "test";
    cfg.providers.push_back(conn);
    cfg.last_used = ursa::LastUsed { "test", "m" };
    return cfg;
}

struct Env {
    PostPump pump;
    std::vector<ursa::ChatRequest> requests;
    std::vector<ursa::ToolCallRequest> ran_tools;
    ursa::StreamFn stream;
    std::shared_ptr<ursa::Session> session = std::make_shared<ursa::Session>();
    ursa::Controller controller {
        std::make_shared<ursa::ApplicationState>(ursa::ApplicationState {
            session, std::make_shared<ursa::ProviderStore>(test_config()),
            std::make_shared<ursa::SubagentManager>(), ursa::get_environment(),
            std::make_shared<ursa::ReviewState>() }),
        pump.fn(), [] { },
        [this](const ursa::ChatRequest& req, const ursa::StreamCallback& cb) {
            return stream(req, cb);
        },
        [this] {
            std::vector<ursa::Tool> tools;
            tools.push_back({ { "bash", "run a shell command",
                                  Json::Value(Json::objectValue) },
                [this](const Json::Value& args) {
                    const std::string raw = args.isString()
                        ? args.asString()
                        : ursa::write_json(args);
                    ran_tools.push_back(
                        ursa::ToolCallRequest { "bash", raw, "", "" });
                    return ursa::ToolOutput { ursa::ToolOutput::Kind::OUTPUT,
                        "ran: " + raw };
                } });
            tools.push_back(
                { { "peek", "read-only probe", Json::Value(Json::objectValue) },
                    [this](const Json::Value& args) {
                        const std::string raw = args.isString()
                            ? args.asString()
                            : ursa::write_json(args);
                        ran_tools.push_back(
                            ursa::ToolCallRequest { "peek", raw, "", "" });
                        return ursa::ToolOutput {
                            ursa::ToolOutput::Kind::OUTPUT, "peeked: " + raw
                        };
                    },
                    ursa::ToolSafety::READ_ONLY });
            tools.push_back(ursa::make_subagent_tool());
            return tools;
        }()
    };

    const ursa::ChatRequest& last_request() const { return requests.back(); }

    size_t user_turn_count() const
    {
        size_t n = 0;
        for (const auto& it : session->items()) {
            if (std::holds_alternative<ursa::UserTurn>(it)) {
                ++n;
            }
        }
        return n;
    }

    const ursa::ToolCall* pending_tool() const
    {
        for (auto it = session->items().rbegin(); it != session->items().rend();
            ++it) {
            if (const auto* tc = std::get_if<ursa::ToolCall>(&*it)) {
                return tc;
            }
        }
        return nullptr;
    }
};

bool showing_tool_ask(const ursa::Session& st)
{
    return std::holds_alternative<ursa::ToolCallRequest>(st.modal())
        && st.phase() == ursa::Session::Phase::AWAITING;
}

bool showing_question(const ursa::Session& st)
{
    return std::holds_alternative<ursa::QuestionForm>(st.modal())
        && st.phase() == ursa::Session::Phase::AWAITING;
}

bool idle(const ursa::Session& st)
{
    return st.phase() == ursa::Session::Phase::IDLE && st.modal().index() == 0;
}

} // namespace

TEST_CASE("subagent tool waits for a research agent and retains its chat")
{
    Env env;
    env.stream
        = [&env](const ursa::ChatRequest& req, const ursa::StreamCallback& cb) {
              env.requests.push_back(req);
              const std::string last = req.messages.empty()
                  ? std::string { }
                  : req.messages.back().content;
              if (last.starts_with("delegate")) {
                  cb(ursa::make_tool_call_event({ "subagent",
                      R"({"tasks":[{"mode":"research","prompt":"inspect"}]})",
                      "delegate inspection", "delegate-1" }));
              } else if (last.starts_with("inspect")) {
                  cb(ursa::make_delta_event("research report"));
              } else {
                  cb(ursa::make_delta_event("main complete"));
              }
              cb(ursa::make_done_event());
              return ursa::Status::OK;
          };

    env.controller.submit("delegate");
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
    const ursa::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == ursa::ToolCall::Result::Kind::OUTPUT);
    CHECK(call->result->text.find("research report") != std::string::npos);
    REQUIRE(call->subagent_chats.size() == 1);
    CHECK(call->subagent_chats[0].transcript.find("inspect")
        != std::string::npos);
    CHECK(call->subagent_chats[0].transcript.find("research report")
        != std::string::npos);
    const auto child_request = std::find_if(env.requests.begin(),
        env.requests.end(), [](const ursa::ChatRequest& request) {
            return !request.messages.empty()
                && request.messages.back().content.starts_with("inspect");
        });
    REQUIRE(child_request != env.requests.end());
    REQUIRE(child_request->messages.size() >= 2);
    CHECK(child_request->messages.front().type == ursa::Message::Type::SYSTEM);
    CHECK(child_request->messages.front().content.find("Ursa subagent")
        != std::string::npos);
    CHECK(child_request->messages.front().content.find("Work read-only")
        != std::string::npos);
    CHECK(child_request->messages.front().content.find("# Todo list")
        == std::string::npos);
    CHECK(child_request->messages.back().content == "inspect");
    CHECK(std::none_of(child_request->tools.begin(), child_request->tools.end(),
        [](const ursa::ToolSpec& tool) {
            return tool.name == "subagent" || tool.name == "todo";
        }));
}

TEST_CASE("subagent tool captures two concurrent agents separately")
{
    Env env;
    env.stream = [](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        const std::string last = req.messages.empty()
            ? std::string { }
            : req.messages.back().content;
        if (last.starts_with("delegate two")) {
            cb(ursa::make_tool_call_event({ "subagent",
                R"({"tasks":[{"mode":"research","prompt":"alpha"},{"mode":"research","prompt":"beta"}]})",
                "delegate two checks", "delegate-2" }));
        } else if (last.starts_with("alpha")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            cb(ursa::make_delta_event("alpha report"));
        } else if (last.starts_with("beta")) {
            cb(ursa::make_delta_event("beta report"));
        } else {
            cb(ursa::make_delta_event("main joined reports"));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("delegate two");
    REQUIRE(env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; }));
    const ursa::ToolCall& call = *env.pending_tool();
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
    const ursa::SubagentChat first  = env.controller.subagent_chat(call, 0);
    const ursa::SubagentChat second = env.controller.subagent_chat(call, 1);
    CHECK(first.title == "Agent 1 (research)");
    CHECK(second.title == "Agent 2 (research)");
    CHECK(first.transcript != second.transcript);

    auto view_state     = std::make_shared<ursa::ApplicationState>();
    view_state->session = env.session;
    auto chat           = ursa::make_chat(view_state, env.controller,
        [] { return ursa::LayoutCtx { ursa::LayoutCtx::Kind::WIDE, 100 }; });
    auto click_agent    = [&](std::string_view label) {
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(50));
        ftxui::Render(screen, chat->Render());
        const std::vector<std::string> lines
            = ursa::split_lines(screen.ToString());
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
    REQUIRE(std::holds_alternative<ursa::ViewerModal>(env.session->modal()));
    CHECK(std::get<ursa::ViewerModal>(env.session->modal()).title
        == "Agent 1 (research)");
    env.controller.close_modal();
    REQUIRE(click_agent("View Agent 2"));
    REQUIRE(std::holds_alternative<ursa::ViewerModal>(env.session->modal()));
    CHECK(std::get<ursa::ViewerModal>(env.session->modal()).title
        == "Agent 2 (research)");
    env.controller.close_modal();
}

TEST_CASE("delegated-agent approvals surface through the main modal queue")
{
    Env env;
    env.stream = [](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        const bool child = std::any_of(req.messages.begin(), req.messages.end(),
            [](const ursa::Message& message) {
                return message.type == ursa::Message::Type::USER
                    && message.content.starts_with("inspect");
            });
        const bool has_tool_result = std::any_of(req.messages.begin(),
            req.messages.end(), [](const ursa::Message& message) {
                return message.type == ursa::Message::Type::TOOL;
            });
        if (!child && !has_tool_result) {
            cb(ursa::make_tool_call_event({ "subagent",
                R"({"tasks":[{"mode":"research","prompt":"inspect"}]})",
                "delegate inspection", "delegate-1" }));
        } else if (child && !has_tool_result) {
            cb(ursa::make_tool_call_event({ "shell",
                R"({"command":"echo child"})", "run probe", "child-shell" }));
        } else {
            cb(ursa::make_delta_event(
                child ? "child approved" : "main complete"));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("delegate");
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<ursa::ToolCallRequest>(
            env.session->modal());
    }));
    const auto request = std::get<ursa::ToolCallRequest>(env.session->modal());
    CHECK(request.description.find("Agent 1 (research)") != std::string::npos);
    env.controller.resolve_modal(
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT, "" });
    REQUIRE(env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; }));
    const ursa::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->text.find("child approved") != std::string::npos);
}

TEST_CASE("subagent failure reports preserve the last completed tool output")
{
    Env env;
    env.stream = [](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        const bool child = std::any_of(req.messages.begin(), req.messages.end(),
            [](const ursa::Message& message) {
                return message.type == ursa::Message::Type::USER
                    && message.content.starts_with("failing child");
            });
        const bool has_tool_result = std::any_of(req.messages.begin(),
            req.messages.end(), [](const ursa::Message& message) {
                return message.type == ursa::Message::Type::TOOL;
            });
        if (!child && !has_tool_result) {
            cb(ursa::make_tool_call_event({ "subagent",
                R"({"tasks":[{"mode":"research","prompt":"failing child"}]})",
                "delegate failing child", "delegate-failure" }));
        } else if (child && !has_tool_result) {
            cb(ursa::make_tool_call_event(
                { "shell", R"({"command":"printf child-output"})",
                    "run command", "child-shell" }));
        } else if (child) {
            cb(ursa::make_error_event(
                ursa::Status::API_ERROR, "follow-up failed"));
        } else {
            cb(ursa::make_delta_event("main complete"));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("delegate failure");
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<ursa::ToolCallRequest>(
            env.session->modal());
    }));
    env.controller.resolve_modal(
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT, "" });
    REQUIRE(env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; }));
    const ursa::ToolCall& call = *env.pending_tool();
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
        = [](const ursa::ChatRequest& req, const ursa::StreamCallback& cb) {
              if (!req.messages.empty()
                  && req.messages.back().content.starts_with("delegate")) {
                  cb(ursa::make_tool_call_event({ "subagent",
                      R"({"tasks":[{"mode":"build","prompt":"change it"}]})",
                      "delegate change", "delegate-1" }));
              } else {
                  cb(ursa::make_delta_event("main complete"));
              }
              cb(ursa::make_done_event());
              return ursa::Status::OK;
          };

    env.controller.submit("delegate");
    const bool finished = env.pump.wait_for(
        [&] { return idle(*env.session) && env.pending_tool() != nullptr; });
    CAPTURE(env.session->items().size());
    CAPTURE(static_cast<int>(env.session->phase()));
    CAPTURE(env.session->error());
    REQUIRE(finished);
    const ursa::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == ursa::ToolCall::Result::Kind::ERROR);
    CHECK(call->result->text.find("require main-agent build mode")
        != std::string::npos);
    CHECK(env.controller.queue_size() == 0);
}

TEST_CASE(
    "question round-trip: AWAITING while pending, reply folded, ask stable")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_delta_event("I need input.\n"));
            cb(ursa::make_question_event(
                { { "Which one?", { "A", "B" }, false, false } }));
        } else {
            cb(ursa::make_delta_event("thanks"));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));
    CHECK(env.controller.queue_size() == 1);

    const std::string ask_md = ursa::question_form_markdown(
        { { "Which one?", { "A", "B" }, false, false } });
    auto assistant_corpus = [&] {
        std::string all;
        for (const auto& it : env.session->items()) {
            if (const auto* a = std::get_if<ursa::AssistantTurn>(&it)) {
                all += a->markdown + "\n";
            }
        }
        return all;
    };
    const std::string snapshot = assistant_corpus();
    CHECK(snapshot.find(ask_md) != std::string::npos);

    env.controller.resolve_modal(
        ursa::ModalResult { ursa::ModalAnswer { { { { "B" }, "", "" } } } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.controller.queue_size() == 0);

    const std::string after = assistant_corpus();
    CHECK(after.find(ask_md) != std::string::npos);
    CHECK(after.find(ask_md) == after.rfind(ask_md));

    size_t answers = 0;
    for (const auto& it : env.session->items()) {
        if (std::holds_alternative<ursa::ModalAnswer>(it)) {
            ++answers;
        }
    }
    REQUIRE(answers == 1);
    CHECK(env.user_turn_count() == 1);
    CHECK(env.last_request().messages.back().type == ursa::Message::Type::USER);
    CHECK(env.last_request().messages.back().content.find("User answered:")
        != std::string::npos);
    CHECK(env.last_request().messages.back().content.find("> B")
        != std::string::npos);
}

TEST_CASE("tool accept: output fills result, request half byte-stable")
{
    Env env;
    env.controller.set_mode(ursa::Session::Mode::BUILD);
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_tool_call_event({ "bash", "ls -la", "list files" }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    const ursa::ToolCall* pending = env.pending_tool();
    REQUIRE(pending != nullptr);
    CHECK(pending->name == "bash");
    CHECK(pending->args == "ls -la");
    CHECK_FALSE(pending->result.has_value());

    env.controller.resolve_modal(ursa::ModalResult {
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    REQUIRE(env.ran_tools.size() == 1);

    const ursa::ToolCall* done = env.pending_tool();
    REQUIRE(done != nullptr);
    CHECK(done->name == "bash");
    CHECK(done->args == "ls -la");
    REQUIRE(done->result.has_value());
    CHECK(done->result->kind == ursa::ToolCall::Result::Kind::OUTPUT);
    CHECK(done->result->text == "ran: ls -la");

    const auto& msgs = env.last_request().messages;
    REQUIRE(msgs.size() >= 2);
    CHECK(msgs.back().type == ursa::Message::Type::TOOL);
    CHECK(msgs.back().content == "ran: ls -la");
    const auto& prev = msgs[msgs.size() - 2];
    CHECK(prev.type == ursa::Message::Type::ASSISTANT);
    REQUIRE(prev.tool_calls.size() == 1);
    CHECK(prev.tool_calls[0].name == "bash");
    CHECK(prev.tool_calls[0].args == "ls -la");

    REQUIRE(env.last_request().tools.size() == 3);
    CHECK(env.last_request().tools[0].name == "bash");
    CHECK(env.last_request().tools[1].name == "peek");
    CHECK(env.last_request().tools[2].name == "subagent");
    CHECK(env.user_turn_count() == 1);
}

TEST_CASE("reject with reason reaches transcript and injected result")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_tool_call_event({ "bash", "rm -rf /", "danger" }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    env.controller.resolve_modal(ursa::ModalResult { ursa::ToolVerdict {
        ursa::ToolDecision::REJECT, "needs approval first" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.ran_tools.empty());

    const ursa::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == ursa::ToolCall::Result::Kind::REJECT);
    CHECK(tc->result->text == "needs approval first");

    CHECK(env.last_request().messages.back().type == ursa::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find(
              "user denied: needs approval first")
        != std::string::npos);
}

TEST_CASE("esc on tool injects generic denial, appends nothing to transcript")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_tool_call_event({ "bash", "ls", "" }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    env.controller.close_modal();

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.ran_tools.empty());

    const ursa::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == ursa::ToolCall::Result::Kind::CANCEL);
    CHECK(env.user_turn_count() == 1);

    CHECK(env.last_request().messages.back().type == ursa::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find("user denied")
        != std::string::npos);
}

TEST_CASE("esc on question skips form, appends nothing, no exception")
{
    Env env;
    env.stream
        = [&env](const ursa::ChatRequest& req, const ursa::StreamCallback& cb) {
              env.requests.push_back(req);
              cb(ursa::make_question_event(
                  { { "Pick", { "x", "y" }, false, false } }));
              cb(ursa::make_done_event());
              return ursa::Status::OK;
          };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));

    env.controller.close_modal();

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.user_turn_count() == 1);
    size_t answers = 0;
    for (const auto& it : env.session->items()) {
        if (std::holds_alternative<ursa::ModalAnswer>(it)) {
            ++answers;
        }
    }
    CHECK(answers == 0);
}

TEST_CASE("one drain cycle folds question answer and tool output correctly")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_question_event(
                { { "Backend?", { "pg", "sqlite" }, false, false } }));
            cb(ursa::make_tool_call_event({ "bash", "whoami", "" }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");

    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));
    CHECK(env.controller.queue_size() == 2);
    env.controller.resolve_modal(
        ursa::ModalResult { ursa::ModalAnswer { { { { "pg" }, "", "" } } } });

    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    env.controller.resolve_modal(ursa::ModalResult {
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    CHECK(env.user_turn_count() == 1);
    const auto& msgs = env.last_request().messages;
    int reply_idx    = -1;
    int tool_idx     = -1;
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].content.find("User answered:") != std::string::npos) {
            reply_idx = static_cast<int>(i);
        }
        if (msgs[i].content.find("ran: whoami") != std::string::npos) {
            tool_idx = static_cast<int>(i);
        }
    }
    REQUIRE(reply_idx >= 0);
    REQUIRE(tool_idx >= 0);
    CHECK(tool_idx < reply_idx);
    CHECK(msgs[tool_idx].type == ursa::Message::Type::TOOL);
    const auto& prev = msgs[tool_idx - 1];
    CHECK(prev.type == ursa::Message::Type::ASSISTANT);
    REQUIRE(prev.tool_calls.size() == 1);
    CHECK(prev.tool_calls[0].name == "bash");
    CHECK(prev.tool_calls[0].args == "whoami");
}

TEST_CASE("FIFO order preserved and queue_size counts overlays")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_question_event({ { "Q1", { "a" }, false, false } }));
            cb(ursa::make_tool_call_event({ "bash", "date", "" }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));

    env.controller.enqueue_user_modal(
        ursa::ViewerModal { "Queued", "content" });
    env.pump.pump();
    CHECK(env.controller.queue_size() == 3);

    env.controller.resolve_modal(
        ursa::ModalResult { ursa::ModalAnswer { { { { "a" }, "", "" } } } });
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    CHECK(env.controller.queue_size() == 2);

    env.controller.close_modal();
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<ursa::ViewerModal>(env.session->modal());
    }));
    CHECK(env.controller.queue_size() == 1);

    env.controller.close_modal();
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.controller.queue_size() == 0);
}

TEST_CASE("accept-always records tool and later calls never queue")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        switch ((*round)++) {
        case 0:
            cb(ursa::make_tool_call_event({ "bash", "echo one", "" }));
            break;
        case 1:
            cb(ursa::make_tool_call_event({ "bash", "echo two", "" }));
            break;
        default: break;
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    env.controller.resolve_modal(ursa::ModalResult {
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT_ALWAYS, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.controller.queue_size() == 0);
    REQUIRE(env.ran_tools.size() == 2);
    CHECK(env.ran_tools[0].args == "echo one");
    CHECK(env.ran_tools[1].args == "echo two");

    size_t tool_items = 0;
    for (const auto& it : env.session->items()) {
        if (const auto* tc = std::get_if<ursa::ToolCall>(&it)) {
            ++tool_items;
            REQUIRE(tc->result.has_value());
            CHECK(tc->result->kind == ursa::ToolCall::Result::Kind::OUTPUT);
        }
    }
    CHECK(tool_items == 2);
    CHECK(env.user_turn_count() == 1);
}

TEST_CASE("user modal enqueued mid-stream surfaces after the ask resolves")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_question_event({ { "Q", { "a" }, false, false } }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));

    env.controller.enqueue_user_modal(
        ursa::VariantModal { { "off", "default" }, "default" });
    env.pump.pump();
    CHECK(std::holds_alternative<ursa::QuestionForm>(env.session->modal()));

    env.controller.resolve_modal(
        ursa::ModalResult { ursa::ModalAnswer { { { { "a" }, "", "" } } } });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<ursa::VariantModal>(env.session->modal());
    }));

    env.controller.close_modal();
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.controller.queue_size() == 0);
}

TEST_CASE("read-only tools run without an approval modal")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_tool_call_event({ "peek", "{}", "", "" }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    CHECK(env.controller.queue_size() == 0);
    REQUIRE(env.ran_tools.size() == 1);
    CHECK(env.ran_tools[0].name == "peek");

    const ursa::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == ursa::ToolCall::Result::Kind::OUTPUT);
    CHECK(tc->result->text == "peeked: {}");

    CHECK(env.last_request().messages.back().type == ursa::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content == "peeked: {}");
}

TEST_CASE("unknown tools error back to the model without a modal")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const ursa::ChatRequest& req,
                     const ursa::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_tool_call_event({ "nope", "{}", "", "" }));
        }
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };

    env.controller.submit("go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    CHECK(env.controller.queue_size() == 0);
    CHECK(env.ran_tools.empty());

    const ursa::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == ursa::ToolCall::Result::Kind::ERROR);
    CHECK(tc->result->text.find("unknown tool: nope") != std::string::npos);

    CHECK(env.last_request().messages.back().type == ursa::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find("unknown tool: nope")
        != std::string::npos);
}
