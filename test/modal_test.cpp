#include <doctest/doctest.h>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/screen.hpp>

#include "app/flows.h"
#include "common/util.h"
#include "conversation/format.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"
#include "tools/tool.h"
#include "turn/delegation.h"
#include "ui/ui.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgentEnv = imza::test::AgentEnv;

bool showing_tool_ask(const imza::Session& st)
{
    return std::holds_alternative<imza::PermissionPrompt>(st.modal())
        && st.phase() == imza::Session::Phase::AWAITING;
}

} // namespace

TEST_CASE("plan requests omit edit and write tools")
{
    AgentEnv env;
    env.stream
        = [&env](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
              env.requests.push_back(req);
              cb(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "inspect");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    REQUIRE_FALSE(env.requests.empty());
    const auto& tools = env.requests.front().tools;
    CHECK(std::none_of(tools.begin(), tools.end(),
        [](const imza::ToolSpec& tool) { return tool.name == "edit"; }));
    CHECK(std::none_of(tools.begin(), tools.end(),
        [](const imza::ToolSpec& tool) { return tool.name == "write"; }));
    CHECK(std::any_of(tools.begin(), tools.end(),
        [](const imza::ToolSpec& tool) { return tool.name == "lua"; }));
}
TEST_CASE("agent shell approval alerts when input is required")
{
    AgentEnv env;
    std::vector<std::string> alerts;
    auto subscription = env.state->notifications->subscribe_to_alert(
        [&alerts](const std::string& message) { alerts.push_back(message); });
    env.stream = [](const imza::ChatRequest& request,
                     const imza::StreamCallback& callback) {
        if (request.messages.back().type == imza::Message::Type::USER) {
            callback(imza::make_tool_call_event({ "lua",
                R"json({"script":"pcall(imza.shell, 'custom notify') print('ran')"})json",
                "", "call" }));
        }
        callback(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "run it");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    REQUIRE(alerts.size() == 1);
    CHECK(alerts.front() == "Imza needs your attention");

    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::REJECT, "" });
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    REQUIRE(alerts.size() == 2);
    CHECK(alerts.back() == "Imza finished");
}
TEST_CASE("subagent titles prefer stored chats and fall back to arguments")
{
    AgentEnv env;
    imza::ToolCall call;
    call.name = "subagent";
    call.args = R"({"tasks":[{"mode":"research","prompt":"a"},)"
                R"({"mode":"build","prompt":"b"},{"prompt":"c"}]})";
    CHECK(
        env.state->delegation->subagent_title(call, 0) == "Agent 1 (research)");
    CHECK(env.state->delegation->subagent_title(call, 1) == "Agent 2 (build)");
    // A task without a recorded mode, and one past the argument list,
    // keep the plain numbering.
    CHECK(env.state->delegation->subagent_title(call, 2) == "Agent 3");
    CHECK(env.state->delegation->subagent_title(call, 9) == "Agent 10");

    call.subagent_chats.push_back(
        imza::SubagentChat { "Stored", "transcript" });
    CHECK(env.state->delegation->subagent_title(call, 0) == "Stored");
}

TEST_CASE("plan mode rejects mutating file operations at the gate")
{
    AgentEnv env;
    const imza::test::TempDir directory;
    const std::filesystem::path out_path = directory.file("out.txt");
    auto round                           = std::make_shared<int>(0);
    env.stream = [out_path, round](
                     const imza::ChatRequest&, const imza::StreamCallback& cb) {
        if ((*round)++ == 0) {
            imza::JsonValue arguments = imza::JsonValue::object_t { };
            arguments["script"]
                = "imza.fs.write([[" + out_path.string() + "]], 'no')";
            cb(imza::make_tool_call_event(
                { "lua", imza::json_dump(arguments), "", "lua-call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "write");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    // The roster-level gate accepts a lua call, so the Plan-mode refusal
    // surfaces as a raised binding error that aborts the script.
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::ERROR);
    CHECK(call->result->text.find("fs.write: denied") != std::string::npos);
    CHECK_FALSE(std::filesystem::is_regular_file(out_path));
}
TEST_CASE("subagent tool captures two concurrent agents separately")
{
    AgentEnv env;
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
    REQUIRE(env.pump.wait_for([&] {
        return imza::test::idle(*env.session) && env.pending_tool() != nullptr;
    }));
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

    auto chat = imza::make_chat(env.state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100 }; });

    REQUIRE(imza::test::click_label(chat, "View Agent 1", 120, 50));
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title
        == "Agent 1 (research)");
    imza::close_modal(*env.state);
    REQUIRE(imza::test::click_label(chat, "View Agent 2", 120, 50));
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title
        == "Agent 2 (research)");
    imza::close_modal(*env.state);
}

TEST_CASE("delegated-agent approvals surface through the main modal queue")
{
    AgentEnv env;
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
                R"json({"script":"print(pcall(imza.shell, 'probe child') == false)"})json",
                "probe child", "child-lua" }));
        } else {
            cb(imza::make_delta_event(
                child ? "child approved" : "main complete"));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "delegate");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    const auto request = std::get<imza::PermissionPrompt>(env.session->modal());
    CHECK(request.description.find("research") != std::string::npos);
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    REQUIRE(env.pump.wait_for([&] {
        return imza::test::idle(*env.session) && env.pending_tool() != nullptr;
    }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->text.find("child approved") != std::string::npos);
}

TEST_CASE("subagent failure reports preserve the last completed tool output")
{
    AgentEnv env;
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
                R"json({"script":"pcall(imza.shell, 'custom child') print('child-output')"})json",
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
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    REQUIRE(env.pump.wait_for([&] {
        return imza::test::idle(*env.session) && env.pending_tool() != nullptr;
    }));
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
    AgentEnv env;
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
    const bool finished = env.pump.wait_for([&] {
        return imza::test::idle(*env.session) && env.pending_tool() != nullptr;
    });
    REQUIRE(finished);
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::REJECT);
    CHECK(call->result->text.find("require main-agent build mode")
        != std::string::npos);
    CHECK(env.state->session->queue.size() == 0);
}

TEST_CASE("tool accept: output fills result, request half byte-stable")
{
    AgentEnv env;
    env.session->set_mode(imza::Session::Mode::BUILD);
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = imza.shell('inspect -la') print(out) print('shell-complete')"})json",
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

    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
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
    AgentEnv env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"imza.shell('git push origin main')"})json",
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

    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::ERROR);
    CHECK(tc->result->text.find("shell: needs approval first")
        != std::string::npos);

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find(
              "shell: needs approval first")
        != std::string::npos);
}

TEST_CASE("esc on tool injects generic denial, appends nothing to transcript")
{
    AgentEnv env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"imza.shell('inspect')"})json", "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    imza::close_modal(*env.state);

    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::ERROR);
    CHECK(tc->result->text.find("shell: permission dismissed")
        != std::string::npos);
    CHECK(env.user_turn_count() == 1);

    REQUIRE(env.requests.size() == 1);
    CHECK(env.state->session->queue.size() == 0);
}

TEST_CASE("FIFO order preserved and queue_size counts overlays")
{
    AgentEnv env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ < 2) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = imza.shell('cmake --build build') print(code)"})json",
                "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    imza::enqueue_user_modal(
        *env.state, imza::ViewerModal { "Queued", "content" });
    env.pump.pump();
    CHECK(env.state->session->queue.size() == 2);

    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" } });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ViewerModal>(env.session->modal())
            && env.state->session->queue.size() == 2;
    }));
    CHECK(env.state->session->queue.size() == 2);

    imza::close_modal(*env.state);
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    CHECK(env.state->session->queue.size() == 1);

    imza::close_modal(*env.state);
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    CHECK(env.state->session->queue.size() == 0);
}
TEST_CASE("one-time shell approval does not authorize later calls")
{
    AgentEnv env;
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        switch ((*round)++) {
        case 0:
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"pcall(imza.shell, 'custom one') print('ran one')"})json",
                "" }));
            break;
        case 1:
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"print(select(2, pcall(imza.shell, 'custom two')))"})json",
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
    CHECK(env.state->session->queue.size() == 1);
    imza::resolve_modal(*env.state,
        imza::ModalResult {
            imza::ToolVerdict { imza::ToolDecision::REJECT, "" } });
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    std::vector<imza::ToolCall::Result> results;
    const auto items = env.session->items();
    for (const auto& it : *items) {
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
    AgentEnv env;
    env.session->set_mode(imza::Session::Mode::BUILD);
    imza::test::TempDir directory;
    const std::filesystem::path path = directory.file("approved.txt");
    auto round                       = std::make_shared<int>(0);
    env.stream = [path, round](
                     const imza::ChatRequest&, const imza::StreamCallback& cb) {
        if ((*round)++ < 2) {
            imza::JsonValue arguments = imza::JsonValue::object_t { };
            arguments["script"] = "assert(not imza.fs.write([[" + path.string()
                + "]], 'approved'))";
            cb(imza::make_tool_call_event(
                { "lua", imza::json_dump(arguments), "", "lua-call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));
    const auto prompt = std::get<imza::PermissionPrompt>(env.session->modal());
    CHECK(prompt.name == "write");
    CHECK(prompt.allow_for_session);
    CHECK(prompt.target == path.string());
    imza::resolve_modal(*env.state,
        imza::ToolVerdict { imza::ToolDecision::ACCEPT_FOR_SESSION, "" });

    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    CHECK(env.state->session->queue.size() == 0);
    CHECK(env.state->permissions->snapshot()->size() == 1);
    const auto lua_items = env.session->items();
    for (const auto& item : *lua_items) {
        if (const auto* call = std::get_if<imza::ToolCall>(&item);
            call != nullptr && call->name == "lua" && call->result) {
            CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
        }
    }
    CHECK(std::filesystem::is_regular_file(path));
}

TEST_CASE("dangerous skip accepts permission-gated tools without a modal")
{
    AgentEnv env(static_cast<imza::RuntimeFlag>(
        imza::ATTENDED | imza::SHELL | imza::SKIP_PERMISSIONS));
    env.stream = [](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        if (req.messages.back().type == imza::Message::Type::USER) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"local out, code = imza.shell('true') print(code)"})json",
                "", "call" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
    CHECK(env.state->session->queue.size() == 0);
    CHECK_FALSE(env.state->runner->blocked_permission());
}

TEST_CASE("dangerous skip does not weaken hard rejection")
{
    AgentEnv env(
        static_cast<imza::RuntimeFlag>(imza::SHELL | imza::SKIP_PERMISSIONS));
    env.stream
        = [](const imza::ChatRequest& req, const imza::StreamCallback& cb) {
              if (req.messages.back().type == imza::Message::Type::USER) {
                  cb(imza::make_tool_call_event({ "lua",
                      R"json({"script":"imza.shell('')"})json", "", "call" }));
              }
              cb(imza::make_done_event());
              return imza::Status::OK;
          };
    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    CHECK(env.state->session->queue.size() == 0);
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::ERROR);
    CHECK(call->result->text.find("empty command") != std::string::npos);
}

TEST_CASE("tools with an automatic policy run without an approval modal")
{
    AgentEnv env(static_cast<imza::RuntimeFlag>(
        imza::interactive_runtime_flags() | imza::RuntimeFlag::WEB));
    auto round = std::make_shared<int>(0);
    env.stream = [&env, round](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"pcall(imza.web.search, 'imza') print('searched: imza')"})json",
                "", "" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    CHECK(env.state->session->queue.size() == 0);

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
    AgentEnv env;
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
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    CHECK(env.state->session->queue.size() == 0);

    const imza::ToolCall* tc = env.pending_tool();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->result.has_value());
    CHECK(tc->result->kind == imza::ToolCall::Result::Kind::ERROR);
    CHECK(tc->result->text.find("unknown tool: nope") != std::string::npos);

    CHECK(env.last_request().messages.back().type == imza::Message::Type::TOOL);
    CHECK(env.last_request().messages.back().content.find("unknown tool: nope")
        != std::string::npos);
}
TEST_CASE("skill approval modal shows the skill identity and its choices")
{
    AgentEnv env;
    imza::PermissionPrompt prompt;
    prompt.name              = "skill";
    prompt.description       = "Load skill docs";
    prompt.reason            = "skill instructions require approval";
    prompt.allow_for_session = true;
    prompt.id                = "manual-skill";
    prompt.request           = imza::SkillRequest { "docs", "project",
        "/tmp/imza-skill-baseline/SKILL.md" };

    imza::enqueue_user_modal(*env.state, prompt);
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::PermissionPrompt>(
            env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    const std::string flat = imza::test::collapse_whitespace(
        imza::test::without_ansi(imza::test::to_text(modal->Render(), 60, 14)));
    imza::close_modal(*env.state);

    // The dialog names what is approved and offers every decision; exact
    // padding and ordering are presentation, not contract.
    CHECK(flat.find("Load skill docs") != std::string::npos);
    CHECK(
        flat.find("skill instructions require approval") != std::string::npos);
    CHECK(flat.find("docs") != std::string::npos);
    CHECK(flat.find("Allow once") != std::string::npos);
    CHECK(flat.find("Allow for this session") != std::string::npos);
    CHECK(flat.find("Reject") != std::string::npos);
}
TEST_CASE("a modal owns the keyboard until it closes")
{
    AgentEnv env;
    env.session->begin_send("hello");

    imza::PermissionPrompt prompt;
    prompt.name        = "shell";
    prompt.description = "Run it";
    prompt.reason      = "needs approval";
    prompt.id          = "call-1";
    prompt.request
        = imza::ShellRequest { "echo hi", std::chrono::seconds(10), { } };
    imza::enqueue_user_modal(*env.state, prompt);

    // The real composition root, not a hand-built copy.
    ftxui::ScreenInteractive screen
        = ftxui::ScreenInteractive::FixedSize(100, 40);
    auto root = imza::make_repl_root(env.state, &screen);
    (void)root->Render();

    for (const char character : std::string_view("zzblocked")) {
        (void)root->OnEvent(ftxui::Event::Character(character));
    }
    CHECK(imza::test::without_ansi(imza::test::to_text(root->Render(), 100, 40))
              .find("zzblocked")
        == std::string::npos);

    imza::close_modal(*env.state);
    (void)root->Render();

    // Once it closes the chat takes keystrokes again. The marker is
    // distinctive because the placeholder already contains ordinary letters.
    for (const char character : std::string_view("zzmarker")) {
        REQUIRE(root->OnEvent(ftxui::Event::Character(character)));
    }
    CHECK(imza::test::without_ansi(imza::test::to_text(root->Render(), 100, 40))
              .find("zzmarker")
        != std::string::npos);
}

TEST_CASE("mcp connect approval omits the session-wide choice")
{
    AgentEnv env;
    imza::PermissionPrompt prompt;
    prompt.name              = "load_mcp";
    prompt.description       = "Connect and load MCP server 'cloudflare'";
    prompt.reason            = "connecting to an MCP server requires approval";
    prompt.allow_for_session = false;
    prompt.id                = "call-1";
    prompt.request = imza::McpRequest { "cloudflare", "Cloudflare", "" };

    imza::enqueue_user_modal(*env.state, prompt);
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::PermissionPrompt>(
            env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    const std::string flat = imza::test::collapse_whitespace(
        imza::test::without_ansi(imza::test::to_text(modal->Render(), 60, 14)));
    imza::close_modal(*env.state);

    CHECK(flat.find("Connect and load MCP server 'cloudflare'")
        != std::string::npos);
    CHECK(flat.find("cloudflare") != std::string::npos);
    CHECK(flat.find("Allow once") != std::string::npos);
    CHECK(flat.find("Reject") != std::string::npos);
    CHECK(flat.find("Allow for this session") == std::string::npos);
}
