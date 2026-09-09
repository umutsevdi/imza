#include <doctest/doctest.h>
#include <json/json.h>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/screen.hpp>

#include "app/flows.h"
#include "common/util.h"
#include "conversation/format.h"
#include "network/json_io.h"
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
    std::vector<imza::ToolCallRequest> ran_tools;
    imza::StreamFn stream;
    std::shared_ptr<imza::ApplicationState> state;
    std::shared_ptr<imza::Session> session;

    explicit Env(imza::RuntimeFlag flags = imza::interactive_runtime_flags())
    {
        std::vector<imza::Tool> tools;
        tools.push_back({ { "shell", "run a shell command",
                              Json::Value(Json::objectValue) },
            [this](const Json::Value& args) {
                const std::string raw
                    = args.isObject() && args["command"].isString()
                    ? args["command"].asString()
                    : imza::write_json(args);
                ran_tools.push_back(
                    imza::ToolCallRequest { "shell", raw, "", "" });
                return imza::ToolOutput { imza::ToolOutput::Kind::OUTPUT,
                    "ran: " + raw };
            } });
        tools.push_back({ { "websearch", "read-only probe",
                              Json::Value(Json::objectValue) },
            [this](const Json::Value& args) {
                const std::string raw = args.isString()
                    ? args.asString()
                    : imza::write_json(args);
                ran_tools.push_back(
                    imza::ToolCallRequest { "websearch", raw, "", "" });
                return imza::ToolOutput { imza::ToolOutput::Kind::OUTPUT,
                    "searched: " + raw };
            } });
        tools.push_back(imza::make_subagent_tool());
        tools.push_back(imza::make_read_tool());
        tools.push_back(imza::make_edit_tool());
        tools.push_back(imza::make_write_tool());
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
        [](const imza::ToolSpec& tool) { return tool.name == "read"; }));
}

TEST_CASE("plan rejects fabricated write calls")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream
        = [round](const imza::ChatRequest&, const imza::StreamCallback& cb) {
              if ((*round)++ == 0) {
                  cb(imza::make_tool_call_event({ "write",
                      R"({"file_path":"/tmp/imza-plan-write","text":"no"})", "",
                      "write-call" }));
              }
              cb(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "write");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::REJECT);
    CHECK(call->result->text.find("unavailable in Plan mode")
        != std::string::npos);
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
    CHECK(child_request->messages.front().content.find("Imza subagent")
        != std::string::npos);
    CHECK(child_request->messages.front().content.find("Work read-only")
        != std::string::npos);
    CHECK(child_request->messages.front().content.find("# Todo list")
        == std::string::npos);
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
            cb(imza::make_tool_call_event({ "shell",
                R"({"command":"probe child"})", "run probe", "child-shell" }));
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
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"sh -c 'printf child-output'"})",
                    "run command", "child-shell" }));
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
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"inspect -la"})", "list files" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    const imza::ToolCall* pending = env.pending_tool();
    REQUIRE(pending != nullptr);
    CHECK(pending->name == "shell");
    CHECK(pending->args == R"({"command":"inspect -la"})");
    CHECK_FALSE(pending->result.has_value());

    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    REQUIRE(env.ran_tools.size() == 1);

    const imza::ToolCall* done = env.pending_tool();
    REQUIRE(done != nullptr);
    CHECK(done->name == "shell");
    CHECK(done->args == R"({"command":"inspect -la"})");
    REQUIRE(done->result.has_value());
    CHECK(done->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(done->result->text == "ran: inspect -la");

    const auto& msgs = env.last_request().messages;
    REQUIRE(msgs.size() >= 2);
    CHECK(msgs.back().type == imza::Message::Type::TOOL);
    CHECK(msgs.back().content == "ran: inspect -la");
    const auto& prev = msgs[msgs.size() - 2];
    CHECK(prev.type == imza::Message::Type::ASSISTANT);
    REQUIRE(prev.tool_calls.size() == 1);
    CHECK(prev.tool_calls[0].name == "shell");
    CHECK(prev.tool_calls[0].args == R"({"command":"inspect -la"})");

    REQUIRE(env.last_request().tools.size() == 6);
    CHECK(env.last_request().tools[0].name == "shell");
    CHECK(env.last_request().tools[1].name == "websearch");
    CHECK(env.last_request().tools[2].name == "subagent");
    CHECK(env.last_request().tools[3].name == "read");
    CHECK(env.last_request().tools[4].name == "edit");
    CHECK(env.last_request().tools[5].name == "write");
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
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"rm -rf /"})", "danger" }));
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
    CHECK(env.ran_tools.empty());

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::REJECT);
    CHECK(tc->result->text == "needs approval first");

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find(
              "user denied: needs approval first")
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
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"inspect"})", "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    imza::close_modal(*env.state);

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.ran_tools.empty());

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::CANCEL);
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
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"whoami"})", "" }));
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
        if (msgs[i].content.find("ran: whoami") != std::string::npos) {
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
    CHECK(prev.tool_calls[0].name == "shell");
    CHECK(prev.tool_calls[0].args == R"({"command":"whoami"})");
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
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"date"})", "" }));
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
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"custom one"})", "" }));
            break;
        case 1:
            cb(imza::make_tool_call_event(
                { "shell", R"({"command":"custom two"})", "" }));
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
    REQUIRE(env.ran_tools.size() == 1);
    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::REJECT, "" } });
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    std::vector<imza::ToolCall::Result::Kind> result_kinds;
    for (const auto& it : env.session->items()) {
        if (const auto* tc = std::get_if<imza::ToolCall>(&it)) {
            REQUIRE(tc->result.has_value());
            result_kinds.push_back(tc->result->kind);
        }
    }
    REQUIRE(result_kinds.size() == 2);
    CHECK(result_kinds[0] == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(result_kinds[1] == imza::ToolCall::Result::Kind::REJECT);
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
        / ("imza-phase4-modal-" + std::to_string(stamp));
    const std::filesystem::path path = directory / "approved.txt";
    std::filesystem::create_directories(directory);
    auto round = std::make_shared<int>(0);
    env.stream = [path, round](
                     const imza::ChatRequest&, const imza::StreamCallback& cb) {
        if ((*round)++ < 2) {
            Json::Value arguments(Json::objectValue);
            arguments["file_path"] = path.string();
            arguments["text"]      = "approved";
            cb(imza::make_tool_call_event(
                { "write", imza::write_json(arguments), "", "write-call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    const auto request = std::get<imza::ToolCallRequest>(env.session->modal());
    CHECK(request.allow_for_session);
    imza::resolve_modal(*env.state,
        imza::ToolVerdict { imza::ToolDecision::ACCEPT_FOR_SESSION, "" });

    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.state->queue.size() == 0);
    CHECK(env.state->permissions->snapshot()->size() == 1);
    for (const auto& item : env.session->items()) {
        if (const auto* call = std::get_if<imza::ToolCall>(&item);
            call != nullptr && call->name == "write" && call->result) {
            CAPTURE(call->result->text);
            CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
        }
    }
    CHECK(std::filesystem::is_regular_file(path));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST_CASE("permission resolution follows attended and dangerous-skip flags")
{
    SUBCASE("attended asks")
    {
        Env env(static_cast<imza::RuntimeFlag>(imza::ATTENDED | imza::SHELL));
        env.stream
            = [](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
                  if (req.messages.back().type == imza::Message::Type::USER) {
                      cb(imza::make_tool_call_event({ "shell",
                          R"({"command":"custom attended"})", "", "call" }));
                  }
                  cb(imza::make_done_event());
                  return imza::Status::OK;
              };
        imza::submit(*env.state, "go");
        REQUIRE(
            env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
        CHECK(env.ran_tools.empty());
        imza::resolve_modal(
            *env.state, imza::ToolVerdict { imza::ToolDecision::REJECT, "" });
        REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
        CHECK_FALSE(env.state->runner->blocked_permission());
    }

    SUBCASE("attended dangerous skip accepts")
    {
        Env env(static_cast<imza::RuntimeFlag>(
            imza::ATTENDED | imza::SHELL | imza::SKIP_PERMISSIONS));
        env.stream
            = [](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
                  if (req.messages.back().type == imza::Message::Type::USER) {
                      cb(imza::make_tool_call_event({ "shell",
                          R"({"command":"custom skipped"})", "", "call" }));
                  }
                  cb(imza::make_done_event());
                  return imza::Status::OK;
              };
        imza::submit(*env.state, "go");
        REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
        REQUIRE(env.ran_tools.size() == 1);
        CHECK(env.state->queue.size() == 0);
    }

    SUBCASE("unattended rejects without a modal")
    {
        Env env(imza::SHELL);
        env.stream
            = [](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
                  if (req.messages.back().type == imza::Message::Type::USER) {
                      cb(imza::make_tool_call_event({ "shell",
                          R"({"command":"custom blocked"})", "", "call" }));
                  }
                  cb(imza::make_done_event());
                  return imza::Status::OK;
              };
        imza::submit(*env.state, "go");
        REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
        CHECK(env.ran_tools.empty());
        CHECK(env.state->queue.size() == 0);
        CHECK(env.state->runner->blocked_permission());
    }

    SUBCASE("unattended dangerous skip accepts")
    {
        Env env(static_cast<imza::RuntimeFlag>(
            imza::SHELL | imza::SKIP_PERMISSIONS));
        env.stream
            = [](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
                  if (req.messages.back().type == imza::Message::Type::USER) {
                      cb(imza::make_tool_call_event({ "shell",
                          R"({"command":"custom skipped"})", "", "call" }));
                  }
                  cb(imza::make_done_event());
                  return imza::Status::OK;
              };
        imza::submit(*env.state, "go");
        REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
        REQUIRE(env.ran_tools.size() == 1);
        CHECK(env.state->queue.size() == 0);
        CHECK_FALSE(env.state->runner->blocked_permission());
    }
}

TEST_CASE("dangerous skip does not weaken hard rejection")
{
    Env env(
        static_cast<imza::RuntimeFlag>(imza::SHELL | imza::SKIP_PERMISSIONS));
    env.stream
        = [](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
              if (req.messages.back().type == imza::Message::Type::USER) {
                  cb(imza::make_tool_call_event(
                      { "shell", R"({"command":""})", "", "call" }));
              }
              cb(imza::make_done_event());
              return imza::Status::OK;
          };
    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.ran_tools.empty());
    CHECK(env.state->queue.size() == 0);
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::REJECT);
}

TEST_CASE("user modal enqueued mid-stream surfaces after the ask resolves")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_question_event({ { "Q", { "a" }, false, false } }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_question(*env.session); }));

    imza::enqueue_user_modal(
        *env.state, imza::VariantModal { { "off", "default" }, "default" });
    env.pump.pump();
    CHECK(std::holds_alternative<imza::QuestionForm>(env.session->modal()));

    imza::resolve_modal(*env.state,
        imza::ModalResult { imza::ModalAnswer { { { { "a" }, "", "" } } } });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::VariantModal>(env.session->modal());
    }));

    imza::close_modal(*env.state);
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.state->queue.size() == 0);
}

TEST_CASE("tools with an automatic policy run without an approval modal")
{
    Env env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event(
                { "websearch", R"({"query":"imza"})", "", "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));

    CHECK(env.state->queue.size() == 0);
    REQUIRE(env.ran_tools.size() == 1);
    CHECK(env.ran_tools[0].name == "websearch");

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(tc->result->text == R"(searched: {"query":"imza"})");

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content
        == R"(searched: {"query":"imza"})");
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
    CHECK(env.ran_tools.empty());

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::ERROR);
    CHECK(tc->result->text.find("unknown tool: nope") != std::string::npos);

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find("unknown tool: nope")
        != std::string::npos);
}
