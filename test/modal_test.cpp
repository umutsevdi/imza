#include <doctest/doctest.h>
#include <json/json.h>

#include "format.h"
#include "tools.h"
#include "ui.h"

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

struct Env {
    PostPump pump;
    std::vector<ursa::ChatRequest> requests;
    std::vector<ursa::ToolCallRequest> ran_tools;
    ursa::StreamFn stream;
    std::shared_ptr<ursa::Session> session
        = std::make_shared<ursa::Session>();
    ursa::Controller controller { session, [] {
        ursa::Config cfg;
        ursa::Connection conn;
        conn.id          = "test";
        conn.provider_id = "test";
        cfg.providers.push_back(conn);
        cfg.last_used = ursa::LastUsed { "test", "m" };
        return cfg;
    }(), pump.fn(), [] { },
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
                    return ursa::ToolOutput {
                        ursa::ToolOutput::Kind::OUTPUT, "ran: " + raw };
                } });
            tools.push_back({ { "peek", "read-only probe",
                            Json::Value(Json::objectValue) },
                [this](const Json::Value& args) {
                    const std::string raw = args.isString()
                        ? args.asString()
                        : ursa::write_json(args);
                    ran_tools.push_back(
                        ursa::ToolCallRequest { "peek", raw, "", "" });
                    return ursa::ToolOutput {
                        ursa::ToolOutput::Kind::OUTPUT, "peeked: " + raw };
                },
                ursa::ToolSafety::READ_ONLY });
            return tools;
        }() };

    const ursa::ChatRequest& last_request() const { return requests.back(); }

    size_t user_turn_count() const
    {
        size_t n = 0;
        for (const auto& it : controller.session().items()) {
            if (std::holds_alternative<ursa::UserTurn>(it)) {
                ++n;
            }
        }
        return n;
    }

    const ursa::ToolCall* pending_tool() const
    {
        for (auto it = controller.session().items().rbegin();
            it != controller.session().items().rend(); ++it) {
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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_question(env.controller.session()); }));
    CHECK(env.controller.queue_size() == 1);

    const std::string ask_md = ursa::question_form_markdown(
        { { "Which one?", { "A", "B" }, false, false } });
    auto assistant_corpus = [&] {
        std::string all;
        for (const auto& it : env.controller.session().items()) {
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

    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
    CHECK(env.controller.queue_size() == 0);

    const std::string after = assistant_corpus();
    CHECK(after.find(ask_md) != std::string::npos);
    CHECK(after.find(ask_md) == after.rfind(ask_md));

    size_t answers = 0;
    for (const auto& it : env.controller.session().items()) {
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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_tool_ask(env.controller.session()); }));

    const ursa::ToolCall* pending = env.pending_tool();
    REQUIRE(pending != nullptr);
    CHECK(pending->name == "bash");
    CHECK(pending->args == "ls -la");
    CHECK_FALSE(pending->result.has_value());

    env.controller.resolve_modal(ursa::ModalResult {
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
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

    REQUIRE(env.last_request().tools.size() == 2);
    CHECK(env.last_request().tools[0].name == "bash");
    CHECK(env.last_request().tools[1].name == "peek");
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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_tool_ask(env.controller.session()); }));

    env.controller.resolve_modal(ursa::ModalResult { ursa::ToolVerdict {
        ursa::ToolDecision::REJECT, "needs approval first" } });

    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_tool_ask(env.controller.session()); }));

    env.controller.close_modal();

    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_question(env.controller.session()); }));

    env.controller.close_modal();

    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
    CHECK(env.user_turn_count() == 1);
    size_t answers = 0;
    for (const auto& it : env.controller.session().items()) {
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

    REQUIRE(env.pump.wait_for(
        [&] { return showing_question(env.controller.session()); }));
    CHECK(env.controller.queue_size() == 2);
    env.controller.resolve_modal(
        ursa::ModalResult { ursa::ModalAnswer { { { { "pg" }, "", "" } } } });

    REQUIRE(env.pump.wait_for(
        [&] { return showing_tool_ask(env.controller.session()); }));
    env.controller.resolve_modal(ursa::ModalResult {
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));

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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_question(env.controller.session()); }));

    env.controller.enqueue_user_modal(
        ursa::ViewerModal { "Queued", "content" });
    env.pump.pump();
    CHECK(env.controller.queue_size() == 3);

    env.controller.resolve_modal(
        ursa::ModalResult { ursa::ModalAnswer { { { { "a" }, "", "" } } } });
    REQUIRE(env.pump.wait_for(
        [&] { return showing_tool_ask(env.controller.session()); }));
    CHECK(env.controller.queue_size() == 2);

    env.controller.close_modal();
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<ursa::ViewerModal>(
            env.controller.session().modal());
    }));
    CHECK(env.controller.queue_size() == 1);

    env.controller.close_modal();
    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_tool_ask(env.controller.session()); }));

    env.controller.resolve_modal(ursa::ModalResult {
        ursa::ToolVerdict { ursa::ToolDecision::ACCEPT_ALWAYS, "" } });

    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
    CHECK(env.controller.queue_size() == 0);
    REQUIRE(env.ran_tools.size() == 2);
    CHECK(env.ran_tools[0].args == "echo one");
    CHECK(env.ran_tools[1].args == "echo two");

    size_t tool_items = 0;
    for (const auto& it : env.controller.session().items()) {
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
    REQUIRE(env.pump.wait_for(
        [&] { return showing_question(env.controller.session()); }));

    env.controller.enqueue_user_modal(
        ursa::VariantModal { { "off", "default" }, "default" });
    env.pump.pump();
    CHECK(std::holds_alternative<ursa::QuestionForm>(
        env.controller.session().modal()));

    env.controller.resolve_modal(
        ursa::ModalResult { ursa::ModalAnswer { { { { "a" }, "", "" } } } });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<ursa::VariantModal>(
            env.controller.session().modal());
    }));

    env.controller.close_modal();
    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
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
    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));

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
    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));

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
