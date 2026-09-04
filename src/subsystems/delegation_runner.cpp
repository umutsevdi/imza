#include "subsystems/delegation_runner.h"
#include "agent/flows.h"
#include "agent/prompt.h"
#include "common/types.h"
#include "common/util.h"
#include "network/json_io.h"
#include "subsystems/format.h"
#include "subsystems/skill_store.h"

#include <algorithm>
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
            if (!result.output.empty()) {
                output += "\n\n" + result.output;
            }
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
                    transcript
                        += "## Reasoning\n\n" + assistant->reasoning + "\n\n";
                }
                if (!assistant->markdown.empty()) {
                    transcript
                        += "## Assistant\n\n" + assistant->markdown + "\n\n";
                }
            } else if (const auto* tool = std::get_if<ToolCall>(&item)) {
                transcript += "## Tool: " + tool->name + "\n\n";
                transcript += "```json\n" + tool->args + "\n```\n\n";
                if (tool->result) {
                    transcript
                        += "```text\n" + tool->result->text + "\n```\n\n";
                }
            } else if (const auto* answer = std::get_if<ModalAnswer>(&item)) {
                transcript += "## Answer\n\n" + modal_answer_markdown(*answer)
                    + "\n\n";
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
            if (const auto* tool = std::get_if<ToolCall>(&*it); tool != nullptr
                && tool->result.has_value() && !tool->result->text.empty()) {
                return "Last completed tool output:\n\n```text\n"
                    + tool->result->text + "\n```";
            }
        }
        return { };
    }

} // namespace

DelegationRunner::DelegationRunner(ApplicationState& state, PostFn post,
    ModalRequestFn modal_request, TurnRunner& runner)
    : state_(&state)
    , post_(std::move(post))
    , modal_request_(std::move(modal_request))
    , runner_(runner)
{
}

void DelegationRunner::submit_delegated(
    std::string text, const ProviderSelection& selection, Session::Mode mode)
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
    const std::string task = text;
    state_->session->begin_send(std::move(text));
    const std::shared_ptr<Environment> env = state_->environment;
    const Config config                    = state_->providers->config();
    const SubagentRole role                = mode == Session::Mode::PLAN
        ? SubagentRole::RESEARCH
        : SubagentRole::BUILDER;
    std::vector<Message> history {
        { Message::Type::SYSTEM,
            build_subagent_system_prompt(
                env->system().get(), env->workspace().get(), role, &config) },
        { Message::Type::USER, task },
    };
    runner_.spawn(std::move(history), std::move(settings));
}

void DelegationRunner::run_subagents(
    const ToolCallRequest& req, std::vector<Message>& tool_msgs)
{
    std::string validation_error;
    const auto parsed = parse_tasks(
        parse_json(req.args), state_->session->mode(), validation_error);
    if (!parsed) {
        const ToolOutput out { ToolOutput::Kind::ERROR, validation_error };
        post_([this, req, out] {
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
        const std::string label = "Agent " + std::to_string(index + 1) + " ("
            + task.mode_name + ")";
        const ProviderSelection& selected = *selection;
        const std::string prompt          = task.prompt;
        const Session::Mode mode          = task.mode;
        auto child_state                  = make_child_application_state(
            *state_, [](const std::function<void()>& action) { action(); },
            runner_.has_stream_override() ? runner_.stream_fn() : StreamFn { },
            delegated_tools(),
            [modal_request = modal_request_](ModalPayload payload) {
                return modal_request(std::move(payload));
            },
            label);
        std::shared_ptr<Session> child_session = child_state->session;
        sessions.push_back(child_session);
        handles.push_back(state_->subagents->start(
            prompt, selected.model, selected.reasoning_effort, true,
            [this, child_state, selected, prompt, mode](
                const std::stop_token& stop) mutable {
                child_state->delegation->submit_delegated(
                    prompt, selected, mode);
                using namespace std::chrono_literals;
                while (child_state->session->phase() != Session::Phase::IDLE) {
                    if (stop.stop_requested()
                        || state_->session->interrupt_requested()) {
                        interrupt(*child_state);
                    }
                    std::this_thread::sleep_for(20ms);
                }
                const std::optional<AssistantTurn> answer
                    = child_state->session->last_assistant();
                const std::string error = child_state->session->error();
                if (!error.empty()) {
                    return SubagentResult { Status::API_ERROR,
                        last_useful_output(*child_state->session) };
                }
                return SubagentResult { Status::OK,
                    answer ? answer->markdown : std::string { } };
            },
            { }, child_session));
    }

    std::vector<std::size_t> ids;
    ids.reserve(handles.size());
    for (const SubagentHandle& handle : handles) {
        ids.push_back(handle.id);
    }
    post_([this, req, ids] { state_->session->set_tool_subagents(req, ids); });

    std::string output;
    std::vector<SubagentChat> chats;
    for (std::size_t index = 0; index < handles.size(); ++index) {
        const SubagentResult result = handles[index].completion.get();
        if (!output.empty()) {
            output += "\n\n";
        }
        output += task_report(index, (*parsed)[index], result);
        chats.push_back(SubagentChat { "Agent " + std::to_string(index + 1)
                + " (" + (*parsed)[index].mode_name + ")",
            session_transcript(*sessions[index]) });
    }
    if (!validation_error.empty()) {
        if (!output.empty()) {
            output += "\n\n";
        }
        output += validation_error;
    }
    const ToolCall::Result::Kind kind = validation_error.empty()
        ? ToolCall::Result::Kind::OUTPUT
        : ToolCall::Result::Kind::ERROR;
    post_([this, req, kind, output, chats = std::move(chats)]() mutable {
        state_->session->set_tool_subagent_chats(req, std::move(chats));
        state_->session->fill_tool_result(
            req, ToolCall::Result { kind, output });
    });
    tool_msgs.push_back({ Message::Type::TOOL, output, { }, req.id });
}

SubagentChat DelegationRunner::subagent_chat(
    const ToolCall& call, std::size_t index) const
{
    if (index < call.subagent_chats.size()) {
        return call.subagent_chats[index];
    }
    const Json::Value args = parse_json(call.args);
    std::string title      = "Agent " + std::to_string(index + 1);
    if (args["tasks"].isArray() && index < args["tasks"].size()
        && args["tasks"][static_cast<Json::ArrayIndex>(index)]["mode"]
            .isString()) {
        title += " ("
            + args["tasks"][static_cast<Json::ArrayIndex>(index)]["mode"]
                  .asString()
            + ")";
    }
    if (index >= call.subagent_ids.size()) {
        return { std::move(title), "No delegated-agent history is available." };
    }
    return subagent_chat(call.subagent_ids[index], std::move(title));
}

SubagentChat DelegationRunner::subagent_chat(
    std::size_t id, std::string title) const
{
    const std::vector<SubagentTask> tasks = state_->subagents->tasks();
    const auto found = std::find_if(tasks.begin(), tasks.end(),
        [id](const SubagentTask& task) { return task.id == id; });
    if (found == tasks.end()) {
        return { std::move(title), "No delegated-agent history is available." };
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

SubagentHandle DelegationRunner::run_subagent(std::string prompt,
    std::string model, std::string variant, SubagentOptions options,
    SubagentCompleteFn complete)
{
    const auto selection = state_->providers->active_selection();
    Route route          = selection ? selection->route : Route { };
    if (model.empty() && selection) {
        model = selection->model;
    }
    if (variant.empty() && selection) {
        variant = selection->reasoning_effort;
    }
    const std::string task_prompt = prompt;
    if (options.transcript) {
        options.transcript->begin_send(task_prompt);
        options.transcript->append_assistant(model, variant);
    }
    const auto deadline = options.timeout > std::chrono::seconds { 0 }
        ? std::optional { std::chrono::steady_clock::now() + options.timeout }
        : std::nullopt;
    auto transcript     = options.transcript;
    return state_->subagents->start(
        std::move(prompt), model, variant, options.visible,
        [this, task_prompt, model, variant, route = std::move(route),
            transcript, deadline,
            max_output_tokens = options.max_output_tokens](
            const std::stop_token& stop) mutable {
            if (model.empty() || route.api.empty()) {
                if (transcript) {
                    transcript->finish_session(
                        error_text(Status::CONFIG_ERROR));
                }
                return SubagentResult { Status::CONFIG_ERROR, { } };
            }
            ChatRequest req;
            req.model             = model;
            req.max_output_tokens = max_output_tokens;
            req.interrupted       = [stop, deadline] {
                return stop.stop_requested()
                    || (deadline
                        && std::chrono::steady_clock::now() >= *deadline);
            };
            req.messages
                = { { Message::Type::SYSTEM, full_system_prompt(*state_) },
                      { Message::Type::USER, task_prompt } };
            apply_reasoning(req, route.dialect, variant, *state_->providers);
            std::string output;
            const StreamCallback callback = [&](const StreamEvent& event) {
                if (transcript) {
                    transcript->apply(event, ModelPricing { });
                }
                if (event.kind == StreamEvent::Kind::CONTENT_DELTA) {
                    output += event.text;
                }
            };
            Status status = runner_.has_stream_override()
                ? runner_.stream_fn()(req, callback)
                : stream(route, req, callback, nullptr);
            if (stop.stop_requested()) {
                status = Status::CANCELLED;
            } else if (deadline
                && std::chrono::steady_clock::now() >= *deadline) {
                status = Status::TIMEOUT;
            }
            if (transcript) {
                transcript->finish_session(
                    status == Status::OK ? "" : error_text(status));
            }
            return SubagentResult { status, std::move(output) };
        },
        std::move(complete), std::move(transcript));
}

void DelegationRunner::spawn_title(std::string input, TurnSettings settings)
{
    const std::string prompt = title_prompt(input);
    state_->subagents->start(
        prompt, settings.model, settings.reasoning_effort, false,
        [this, prompt, settings = std::move(settings)](
            const std::stop_token& stop) {
            ChatRequest req;
            req.model       = settings.model;
            req.temperature = 0.2;
            req.interrupted = [stop] { return stop.stop_requested(); };
            req.messages    = { { Message::Type::USER, prompt } };
            apply_reasoning(req, settings.dialect, settings.reasoning_effort,
                *state_->providers);
            std::string title;
            const StreamCallback cb = [&](const StreamEvent& event) {
                if (event.kind == StreamEvent::Kind::CONTENT_DELTA
                    && title.size() < 200) {
                    title += event.text;
                }
            };
            Route route         = settings.route;
            const Status status = stream(route, req, cb, nullptr);
            if (status != Status::OK) {
                return SubagentResult { status, { } };
            }
            return SubagentResult { Status::OK, std::move(title) };
        },
        [this](const SubagentResult& result) {
            if (result.status != Status::OK) {
                return;
            }
            std::string title         = std::string(trim(result.output));
            const std::size_t newline = title.find_first_of("\r\n");
            if (newline != std::string::npos) {
                title.erase(newline);
            }
            if (title.size() >= 2
                && ((title.front() == '"' && title.back() == '"')
                    || (title.front() == '\'' && title.back() == '\''))) {
                title = title.substr(1, title.size() - 2);
            }
            if (title.empty()) {
                return;
            }
            post_([this, title = std::move(title)]() mutable {
                state_->session->set_title(std::move(title));
            });
        });
}

} // namespace ursa
