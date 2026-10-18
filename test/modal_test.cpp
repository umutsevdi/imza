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

TEST_CASE("attended root turn alerts once after completion")
{
    AgentEnv env;
    std::vector<std::string> alerts;
    auto subscription = env.state->notifications->subscribe_to_alert(
        [&alerts](const std::string& message) { alerts.push_back(message); });
    env.stream
        = [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
              callback(imza::make_done_event());
              return imza::Status::OK;
          };

    imza::submit(*env.state, "inspect");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    REQUIRE(alerts.size() == 1);
    CHECK(alerts.front() == "Imza finished");

    imza::on_turn_finished(*env.state, "");
    CHECK(alerts.size() == 1);
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

TEST_CASE("queued agent modal alerts only when presented")
{
    AgentEnv env;
    std::vector<std::string> alerts;
    auto subscription = env.state->notifications->subscribe_to_alert(
        [&alerts](const std::string& message) { alerts.push_back(message); });
    imza::enqueue_user_modal(
        *env.state, imza::ViewerModal { "First", "content" });
    auto result = imza::request_modal(
        *env.state, imza::ViewerModal { "Agent", "content" });
    env.pump.pump();

    CHECK(alerts.empty());
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title == "First");

    imza::close_modal(*env.state);
    REQUIRE(std::holds_alternative<imza::ViewerModal>(env.session->modal()));
    CHECK(std::get<imza::ViewerModal>(env.session->modal()).title == "Agent");
    REQUIRE(alerts.size() == 1);
    CHECK(alerts.front() == "Imza needs your attention");

    imza::close_modal(*env.state);
    CHECK(result.get().index() == 0);
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

TEST_CASE("subagent tool waits for a research agent and retains its chat")
{
    AgentEnv env;
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
    const bool finished = env.pump.wait_for([&] {
        return imza::test::idle(*env.session) && env.pending_tool() != nullptr;
    });
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

TEST_CASE("markdown viewer renders markdown instead of source lines")
{
    AgentEnv env;
    imza::enqueue_user_modal(*env.state,
        imza::ViewerModal { "Document", "# Heading\n\n```cpp\nreturn 1;\n```",
            "markdown", 1, true, "" });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ViewerModal>(env.session->modal());
    }));

    ftxui::Component modal     = imza::make_modal(env.state);
    const std::string rendered = imza::test::to_text(modal->Render(), 60, 12);
    CHECK(rendered.find("Heading") != std::string::npos);
    CHECK(rendered.find("# Heading") == std::string::npos);
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

TEST_CASE("modal action buttons respond to mouse clicks")
{
    AgentEnv env;
    env.session->set_mode(imza::Session::Mode::BUILD);
    env.stream = [&env](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) {
        env.requests.push_back(req);
        if (req.messages.back().type == imza::Message::Type::USER) {
            cb(imza::make_tool_call_event({ "lua",
                R"json({"script":"pcall(imza.shell, 'custom click') print('ran')"})json",
                "", "call-1" }));
        }
        cb(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "go");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    ftxui::Component modal = imza::make_modal(env.state);
    CHECK(imza::test::click_label(modal, "Allow once"));
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    const imza::ToolCall* call = env.pending_tool();
    REQUIRE(call != nullptr);
    REQUIRE(call->result.has_value());
    CHECK(call->result->kind == imza::ToolCall::Result::Kind::OUTPUT);
}

TEST_CASE("skill approval modal renders its canonical baseline")
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
    std::string rendered;
    for (const std::string& line :
        imza::split_lines(imza::test::to_text(modal->Render(), 60, 14))) {
        const std::string trimmed
            = std::string(imza::trim(imza::test::without_ansi(line)));
        if (!trimmed.empty()) {
            rendered += trimmed + "\n";
        }
    }
    imza::close_modal(*env.state);
    // Whitespace runs collapse so the pin covers content, order, and labels
    // rather than the header padding: the R2/R4 refactor must keep this
    // dialog equivalent, not byte-identical to a terminal width.
    std::string flat;
    for (const std::string& line : imza::split_lines(rendered)) {
        const std::string collapsed = imza::test::collapse_whitespace(line);
        if (!collapsed.empty()) {
            flat += collapsed + "\n";
        }
    }
    CHECK(flat == R"(Skill 1 remaining
Load skill docs
skill instructions require approval
name docs
path /tmp/imza-skill-baseline/SKILL.md
scope project
Allow once Allow for this session Reject
allow lasts until directory or session changes
Esc reject
)");
}
TEST_CASE("skills modal manages real skills and switches tabs")
{
    imza::test::IsolatedDataHome data_home; // policy commits hit the store
    imza::test::IsolatedConfigHome config_home;
    config_home.add_skill("docs", "Write documentation");
    config_home.add_skill("deploy", "Ship it");

    AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::SkillsModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::SkillsModal>(env.session->modal());
    }));

    ftxui::Component modal   = imza::make_modal(env.state);
    const std::string manage = imza::test::to_text(modal->Render(), 80, 20);
    CHECK(manage.find("Manage") != std::string::npos);
    CHECK(manage.find("Search") != std::string::npos);
    CHECK(manage.find("docs") != std::string::npos);
    CHECK(manage.find("Write documentation") != std::string::npos);
    CHECK(manage.find("deploy") != std::string::npos);
    CHECK(manage.find("Global") != std::string::npos);
    CHECK(manage.find("Ask") != std::string::npos); // default policy
    CHECK(manage.find("Remove") != std::string::npos);

    // Enter activates the focused control, which is the policy chip on a
    // freshly selected row: the list opens below the row.
    REQUIRE(modal->OnEvent(ftxui::Event::Return));
    const std::string opened = imza::test::to_text(modal->Render(), 80, 24);
    CHECK(opened.find("Deny") != std::string::npos);
    CHECK(opened.find("Remove") != std::string::npos);

    // ArrowDown highlights Deny; Space commits it, same as Enter.
    REQUIRE(modal->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(modal->OnEvent(ftxui::Event::Character(' ')));
    const std::string committed = imza::test::to_text(modal->Render(), 80, 24);
    CHECK(env.pump.wait_for([&] {
        return env.state->providers->config().global_skills.at("deploy")
            == imza::SkillPolicy::DENY;
    }));
    CHECK(env.state->providers->config().global_skills.at("docs")
        == imza::SkillPolicy::ASK);

    // Tab switches to the search tab, which shows the query prompt.
    REQUIRE(modal->OnEvent(ftxui::Event::Tab));
    const std::string search = imza::test::to_text(modal->Render(), 80, 20);
    CHECK(search.find("search skills.sh") != std::string::npos);
    imza::close_modal(*env.state);
}

TEST_CASE("skills modal picks up a skill installed after it opened")
{
    imza::test::IsolatedConfigHome config_home;
    config_home.add_skill("docs", "Write documentation");

    AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::SkillsModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::SkillsModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    CHECK(imza::test::to_text(modal->Render(), 80, 20).find("docs")
        != std::string::npos);
    CHECK(imza::test::to_text(modal->Render(), 80, 20).find("fresh")
        == std::string::npos);

    // A skill installed while the modal is open (the search tab's install
    // path) must appear in Manage without reopening it.
    config_home.add_skill("fresh", "Just installed");
    env.state->environment->rescan_skills();
    REQUIRE(env.pump.wait_for([&] {
        return imza::test::to_text(modal->Render(), 80, 20).find("fresh")
            != std::string::npos;
    }));
    imza::close_modal(*env.state);
}
TEST_CASE("skills modal keys are consistent across the policy chip and Remove")
{
    imza::test::IsolatedConfigHome config_home;
    config_home.add_skill("docs", "Write documentation");

    AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::SkillsModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::SkillsModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    (void)modal->Render();

    // Space on the focused policy chip opens the list, exactly like Enter.
    REQUIRE(modal->OnEvent(ftxui::Event::Character(' ')));
    CHECK(imza::test::to_text(modal->Render(), 80, 24).find("Deny")
        != std::string::npos);
    REQUIRE(modal->OnEvent(ftxui::Event::Escape)); // close the list

    // Right moves focus from the chip to Remove; Enter then arms the
    // confirmation, like Space does on a button.
    REQUIRE(modal->OnEvent(ftxui::Event::ArrowRight));
    REQUIRE(modal->OnEvent(ftxui::Event::Return));
    CHECK(imza::test::to_text(modal->Render(), 80, 24).find("Yes")
        != std::string::npos);
    imza::close_modal(*env.state);
}

TEST_CASE("skills modal tab bar does not invert the selected tab")
{
    imza::test::IsolatedConfigHome config_home;
    AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::SkillsModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::SkillsModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    // The tab row carries no inverse-video escape: the bar is rendered
    // directly, so it must not report itself focused and highlight the
    // selected tab with a background.
    const std::string raw = imza::test::to_text(modal->Render(), 80, 20);
    for (const std::string& line : imza::split_lines(raw)) {
        if (imza::test::without_ansi(line).find("Manage")
            != std::string::npos) {
            CHECK(line.find("\x1b[7m") == std::string::npos);
            break;
        }
    }
    imza::close_modal(*env.state);
}

TEST_CASE("subagents modal aligns the model and variant columns")
{
    AgentEnv env;
    imza::enqueue_user_modal(*env.state,
        imza::ConnectModal { imza::ConnectModal::Entry::SUBAGENTS });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ConnectModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    const std::string text = imza::test::without_ansi(
        imza::test::to_text(modal->Render(), 100, 20));

    // The role rows carry the model column; every one must begin at the
    // same offset or the list reads as ragged.
    std::vector<std::size_t> model_columns;
    std::vector<std::size_t> variant_columns;
    for (const std::string& line : imza::split_lines(text)) {
        if (line.find("Subagent  ") == std::string::npos) {
            continue;
        }
        const std::size_t model = line.find("<Default>");
        REQUIRE(model != std::string::npos);
        model_columns.push_back(model);
        // Basic runs with reasoning off, so it has no variant cell; the
        // blank reservation still keeps its model column aligned.
        const std::size_t variant = line.find("< ", model);
        if (variant != std::string::npos) {
            variant_columns.push_back(variant);
        }
    }
    REQUIRE(model_columns.size() == 3);
    CHECK(model_columns[0] == model_columns[1]);
    CHECK(model_columns[1] == model_columns[2]);

    REQUIRE(variant_columns.size() == 2);
    CHECK(variant_columns[0] == variant_columns[1]);

    imza::close_modal(*env.state);
}

TEST_CASE("closing a modal keeps keyboard focus on the chat")
{
    AgentEnv env;
    env.stream = [](const imza::ChatRequest& request,
                     const imza::StreamCallback& callback) {
        if (request.messages.back().type == imza::Message::Type::USER) {
            callback(imza::make_tool_call_event({ "lua",
                R"json({"script":"pcall(imza.shell, 'custom one') print('ran')"})json",
                "" }));
        }
        callback(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "run it");
    REQUIRE(env.pump.wait_for([&] { return showing_tool_ask(*env.session); }));

    // Mirror the Repl composition: a tab container holding the build tab
    // with the chat, modal stacked last, chat focused at construction.
    auto chat = imza::make_chat(env.state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    imza::SidechatStatus sidechat_status;
    auto sidechat
        = imza::make_sidechat_component(env.state, [] { }, sidechat_status);
    auto build = imza::make_build_tab(
        env.state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 }; },
        chat, sidechat, sidechat_status);
    build->Add(chat);
    int selected_pane = 1;
    auto tabs_content = ftxui::Container::Tab({ build }, &selected_pane);
    auto modal        = imza::make_modal(env.state);
    auto root         = ftxui::Container::Stacked(
        { ftxui::Container::Vertical({ tabs_content }), modal });
    chat->TakeFocus();
    REQUIRE(chat->Focused());
    (void)root->Render();

    // Present the modal and dismiss it with a mouse click on the "Allow
    // once" button, like the screen loop does. Modals are typically opened
    // from a sidebar link; that button's TakeFocus rotates the stacked
    // root's active child away from the main column, which kills keyboard
    // flow into the chat.
    auto sidebar = ftxui::Container::Vertical({ });
    sidebar->Add(ftxui::Button("Sessions", [] { }));
    root->Add(sidebar);
    (void)modal->Render();
    REQUIRE(imza::test::click_label(sidebar, "Sessions"));
    REQUIRE(imza::test::click_label(modal, "Allow once"));
    REQUIRE(
        env.pump.wait_for([&] { return env.session->modal().index() == 0; }));
    CHECK_FALSE(chat->Focused());

    // The Repl re-asserts main focus when the modal closes; keystrokes
    // must reach the chat input again.
    chat->TakeFocus();
    REQUIRE(root->OnEvent(ftxui::Event::Character("k")));
    (void)root->Render();
    CHECK(imza::test::without_ansi(imza::test::to_text(root->Render(), 100, 40))
              .find("k")
        != std::string::npos);
}
TEST_CASE(
    "mcp connect approval modal renders its baseline without session approval")
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
    std::string flat;
    for (const std::string& line :
        imza::split_lines(imza::test::to_text(modal->Render(), 60, 14))) {
        const std::string collapsed = imza::test::collapse_whitespace(
            imza::trim(imza::test::without_ansi(line)));
        if (!collapsed.empty()) {
            flat += collapsed + "\n";
        }
    }
    imza::close_modal(*env.state);
    CHECK(flat == R"(Load MCP 1 remaining
Connect and load MCP server 'cloudflare'
connecting to an MCP server requires approval
server cloudflare Cloudflare
Allow once Reject
Esc reject
)");
}

TEST_CASE("variant modal lists efforts and commits the selection")
{
    AgentEnv env;
    imza::enqueue_user_modal(*env.state,
        imza::VariantModal { { "off", "low", "default", "high" }, "default" });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::VariantModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    const std::string list = imza::test::to_text(modal->Render(), 80, 20);
    CHECK(list.find("Reasoning effort") != std::string::npos);
    for (const char* effort : { "off", "low", "default", "high" }) {
        CHECK(list.find(effort) != std::string::npos);
    }

    // "default" is current; one step down lands on "high", and
    // committing persists the effort the status line will show.
    REQUIRE(modal->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(modal->OnEvent(ftxui::Event::Return));
    CHECK(env.pump.wait_for([&] {
        return env.state->providers->status().reasoning_effort == "high";
    }));
}

TEST_CASE("sessions modal lists saved sessions and empty state")
{
    imza::test::IsolatedDataHome data_home;
    AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::SessionsModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::SessionsModal>(
            env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    CHECK(imza::test::to_text(modal->Render(), 80, 20).find("No saved sessions")
        != std::string::npos);
    imza::close_modal(*env.state);

    env.session->set_title("Sessions probe");
    env.session->begin_send("hello");
    env.session->append_assistant("model", "off");
    env.session->apply(imza::make_delta_event("world"), { });
    env.session->finish_session("");
    // Saved through the store: the modal lists what the store sees.
    REQUIRE(env.state->sessions->save(*env.session) == imza::Status::OK);

    imza::enqueue_user_modal(*env.state, imza::SessionsModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::SessionsModal>(
            env.session->modal());
    }));
    const std::string list = imza::test::to_text(modal->Render(), 80, 20);
    CHECK(list.find("Sessions probe") != std::string::npos);
    imza::close_modal(*env.state);
}

TEST_CASE("mcp modal shows the server list and add section")
{
    imza::test::IsolatedDataHome data_home;
    AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::McpModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::McpModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    const std::string list = imza::test::to_text(modal->Render(), 80, 30);
    CHECK(list.find("MCP Servers") != std::string::npos);
    CHECK(list.find("Add Server") != std::string::npos);
    imza::close_modal(*env.state);
}

TEST_CASE("connect modal opens on the connections entry")
{
    AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::ConnectModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::ConnectModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    const std::string list = imza::test::to_text(modal->Render(), 80, 30);
    CHECK(list.find("Connections") != std::string::npos);
    CHECK(list.find("Add Provider") != std::string::npos);
    imza::close_modal(*env.state);
}
