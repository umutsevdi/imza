#include "controller.h"

#include "format.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace ursa {

namespace {

    struct DelegatedTask {
        Session::Mode mode = Session::Mode::PLAN;
        SubagentRole role  = SubagentRole::RESEARCH;
        std::string mode_name;
        std::string prompt;
    };

    std::optional<std::vector<DelegatedTask>> parse_tasks(
        const Json::Value& args, Session::Mode main_mode, std::string& error)
    {
        if (!args.isObject() || !args["tasks"].isArray()
            || args["tasks"].empty() || args["tasks"].size() > 5) {
            error = "subagent: expected one to five tasks";
            return std::nullopt;
        }
        std::vector<DelegatedTask> tasks;
        tasks.reserve(args["tasks"].size());
        for (const Json::Value& value : args["tasks"]) {
            if (!value.isObject() || !value["mode"].isString()
                || !value["prompt"].isString()
                || trim(value["prompt"].asString()).empty()) {
                error = "subagent: every task requires a mode and prompt";
                return std::nullopt;
            }
            const std::string mode = to_lower(value["mode"].asString());
            if (mode != "research" && mode != "build") {
                error = "subagent: mode must be research or build";
                return std::nullopt;
            }
            if (mode == "build" && main_mode != Session::Mode::BUILD) {
                error = "subagent: build agents require main-agent build mode";
                return std::nullopt;
            }
            tasks.push_back(DelegatedTask {
                mode == "build" ? Session::Mode::BUILD : Session::Mode::PLAN,
                mode == "build" ? SubagentRole::BUILDER
                                : SubagentRole::RESEARCH,
                mode, value["prompt"].asString() });
        }
        return tasks;
    }

    std::vector<Tool> delegated_tools()
    {
        std::vector<Tool> tools = default_tools();
        std::erase_if(tools, [](const Tool& tool) {
            return tool.spec.name == "subagent" || tool.spec.name == "todo";
        });
        return tools;
    }

    std::string task_report(std::size_t index, const DelegatedTask& task,
        const SubagentResult& result)
    {
        std::string output = "## Agent " + std::to_string(index + 1) + " ("
            + task.mode_name + ")\n\n";
        if (result.status != Status::OK) {
            output += "Failed: " + error_text(result.status);
            if (!result.output.empty()) output += "\n\n" + result.output;
            return output;
        }
        output += result.output.empty() ? "Completed without a report."
                                        : result.output;
        return output;
    }

    std::string session_transcript(const Session& session)
    {
        std::string transcript;
        const SessionSnapshot snapshot = session.snapshot();
        for (const ConversationItem& item : snapshot.items) {
            if (const auto* user = std::get_if<UserTurn>(&item)) {
                transcript += "## User\n\n" + user->text + "\n\n";
            } else if (const auto* assistant
                = std::get_if<AssistantTurn>(&item)) {
                if (!assistant->reasoning.empty()) {
                    transcript += "## Reasoning\n\n"
                        + assistant->reasoning + "\n\n";
                }
                if (!assistant->markdown.empty()) {
                    transcript += "## Assistant\n\n"
                        + assistant->markdown + "\n\n";
                }
            } else if (const auto* tool = std::get_if<ToolCall>(&item)) {
                transcript += "## Tool: " + tool->name + "\n\n";
                transcript += "```json\n" + tool->args + "\n```\n\n";
                if (tool->result) {
                    transcript += "```text\n" + tool->result->text
                        + "\n```\n\n";
                }
            } else if (const auto* answer
                = std::get_if<ModalAnswer>(&item)) {
                transcript += "## Answer\n\n"
                    + modal_answer_markdown(*answer) + "\n\n";
            }
        }
        return transcript;
    }

    std::string last_useful_output(const Session& session)
    {
        const SessionSnapshot snapshot = session.snapshot();
        for (auto it = snapshot.items.rbegin(); it != snapshot.items.rend();
            ++it) {
            if (const auto* assistant = std::get_if<AssistantTurn>(&*it);
                assistant != nullptr && !assistant->markdown.empty()) {
                return assistant->markdown;
            }
            if (const auto* tool = std::get_if<ToolCall>(&*it);
                tool != nullptr && tool->result.has_value()
                && !tool->result->text.empty()) {
                return "Last completed tool output:\n\n```text\n"
                    + tool->result->text + "\n```";
            }
        }
        return { };
    }

} // namespace

void Controller::submit_delegated(std::string text,
    const ProviderSelection& selection, Session::Mode mode)
{
    TurnSettings settings;
    settings.model            = selection.model;
    settings.reasoning_effort = selection.reasoning_effort;
    settings.connection_id    = selection.connection_id;
    settings.route            = selection.route;
    settings.dialect          = selection.route.dialect;
    settings.mode             = mode;
    state_->session->set_mode(mode);
    state_->session->clear_interrupt();
    state_->session->begin_send(std::move(text));
    _spawn(state_->session->build_history(_system_prompt(), settings.dialect),
        has_stream_override_ ? stream_fn_ : StreamFn { },
        std::move(settings));
}

void Controller::_run_subagents(
    const ToolCallRequest& req, std::vector<Message>& tool_msgs)
{
    std::string validation_error;
    const auto parsed = parse_tasks(
        parse_json(req.args), state_->session->mode(), validation_error);
    if (!parsed) {
        const ToolOutput out { ToolOutput::Kind::ERROR, validation_error };
        _post([this, req, out] {
            state_->session->fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::ERROR, out.text });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, validation_error, { }, req.id });
        return;
    }

    std::vector<SubagentHandle> handles;
    std::vector<std::shared_ptr<Session>> sessions;
    handles.reserve(parsed->size());
    sessions.reserve(parsed->size());

    for (std::size_t index = 0; index < parsed->size(); ++index) {
        const DelegatedTask& task = (*parsed)[index];
        const auto selection = state_->providers->subagent_selection(task.role);
        if (!selection) {
            validation_error = "subagent: no model is available for agent "
                + std::to_string(index + 1);
            break;
        }
        auto child_session = std::make_shared<Session>();
        sessions.push_back(child_session);
        const std::string label = "Agent " + std::to_string(index + 1)
            + " (" + task.mode_name + ")";
        const ProviderSelection selected = *selection;
        const std::string prompt          = task.prompt;
        const Session::Mode mode          = task.mode;
        handles.push_back(state_->subagents->start(prompt, selected.model,
            selected.reasoning_effort, true,
            [this, child_session, selected, prompt, mode, label](
                std::stop_token stop) mutable {
                auto child_state = std::make_shared<ApplicationState>(
                    ApplicationState { child_session, state_->providers,
                        std::make_shared<SubagentManager>(),
                        state_->environment });
                Controller child(child_state,
                    [](std::function<void()> action) { action(); }, [] { },
                    has_stream_override_ ? stream_fn_ : StreamFn { },
                    delegated_tools(),
                    [this](ModalPayload payload) {
                        return _request_modal(std::move(payload));
                    },
                    label);
                child.submit_delegated(prompt, selected, mode);
                using namespace std::chrono_literals;
                while (child_session->phase() != Session::Phase::IDLE) {
                    if (stop.stop_requested()
                        || state_->session->interrupt_requested()) {
                        child.interrupt();
                    }
                    std::this_thread::sleep_for(20ms);
                }
                const std::optional<AssistantTurn> answer
                    = child_session->last_assistant();
                const std::string error = child_session->error();
                if (!error.empty()) {
                    return SubagentResult {
                        Status::API_ERROR, last_useful_output(*child_session) };
                }
                return SubagentResult { Status::OK,
                    answer ? answer->markdown : std::string { } };
            },
            { }, child_session));
    }

    std::vector<std::size_t> ids;
    ids.reserve(handles.size());
    for (const SubagentHandle& handle : handles) ids.push_back(handle.id);
    _post([this, req, ids] { state_->session->set_tool_subagents(req, ids); });

    std::string output;
    std::vector<SubagentChat> chats;
    for (std::size_t index = 0; index < handles.size(); ++index) {
        const SubagentResult result = handles[index].completion.get();
        if (!output.empty()) output += "\n\n";
        output += task_report(index, (*parsed)[index], result);
        chats.push_back(SubagentChat {
            "Agent " + std::to_string(index + 1) + " ("
                + (*parsed)[index].mode_name + ")",
            session_transcript(*sessions[index]) });
    }
    if (!validation_error.empty()) {
        if (!output.empty()) output += "\n\n";
        output += validation_error;
    }
    const ToolCall::Result::Kind kind = validation_error.empty()
        ? ToolCall::Result::Kind::OUTPUT
        : ToolCall::Result::Kind::ERROR;
    _post([this, req, kind, output, chats = std::move(chats)]() mutable {
        state_->session->set_tool_subagent_chats(req, std::move(chats));
        state_->session->fill_tool_result(
            req, ToolCall::Result { kind, output });
    });
    tool_msgs.push_back({ Message::Type::TOOL, output, { }, req.id });
}

SubagentChat Controller::subagent_chat(
    const ToolCall& call, std::size_t index) const
{
    if (index < call.subagent_chats.size()) {
        return call.subagent_chats[index];
    }
    const Json::Value args = parse_json(call.args);
    std::string title = "Agent " + std::to_string(index + 1);
    if (args["tasks"].isArray() && index < args["tasks"].size()
        && args["tasks"][static_cast<Json::ArrayIndex>(index)]["mode"]
               .isString()) {
        title += " ("
            + args["tasks"][static_cast<Json::ArrayIndex>(index)]["mode"]
                  .asString()
            + ")";
    }
    if (index >= call.subagent_ids.size()) {
        return { std::move(title),
            "No delegated-agent history is available." };
    }
    const std::size_t id = call.subagent_ids[index];
    const std::vector<SubagentTask> tasks = state_->subagents->tasks();
    const auto found = std::find_if(tasks.begin(), tasks.end(),
        [id](const SubagentTask& task) { return task.id == id; });
    if (found == tasks.end()) {
        return { std::move(title),
            "No delegated-agent history is available." };
    }
    std::string transcript = "**Task:** " + found->prompt + "\n\n";
    if (found->session) {
        transcript += session_transcript(*found->session);
    } else {
        transcript += found->output;
    }
    if (found->state == SubagentTask::State::RUNNING) {
        transcript += "_Agent is still running._\n";
    }
    return { std::move(title), std::move(transcript) };
}

} // namespace ursa
