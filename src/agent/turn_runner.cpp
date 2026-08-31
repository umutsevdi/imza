#include "agent/turn_runner.h"
#include "agent/format.h"
#include "agent/subsystems/skill_store.h"
#include "environment/environment.h"
#include "network/json_io.h"
#include "provider/pricing.h"
#include "provider/provider_store.h"
#include "common/types.h"
#include "common/util.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ursa {

namespace {

    constexpr std::uint64_t COMPACTION_PERCENT = 80;

    std::string compaction_transcript(
        const std::vector<Message>& history, std::size_t end)
    {
        std::string out;
        for (std::size_t index = 1; index < end; ++index) {
            const Message& message = history[index];
            out += message.type == Message::Type::USER     ? "\nUSER:\n"
                : message.type == Message::Type::ASSISTANT ? "\nASSISTANT:\n"
                                                           : "\nTOOL:\n";
            out += message.content;
            for (const auto& call : message.tool_calls) {
                out += "\nTOOL CALL " + call.name + ": " + call.args;
            }
        }
        return out;
    }

} // namespace

void apply_reasoning(ChatRequest& req, ApiStandard dialect,
    std::string_view effort, const ProviderStore& providers)
{
    req.reasoning_effort.reset();
    req.thinking_budget.reset();
    const std::string configured = to_config_effort(effort);
    if (configured == "off" || req.model.empty()
        || !providers.model_reasons(req.model)) {
        return;
    }
    if (dialect == ApiStandard::ANTHROPIC) {
        req.thinking_budget = configured == "low"    ? 2000
            : configured == "high" ? 16000
                                   : 8000;
    } else {
        req.reasoning_effort = to_wire_effort(configured);
    }
}

TurnRunner::TurnRunner(ApplicationState& state, PostFn post,
    std::vector<Tool> tools, StreamFn stream_fn, ModalRequestFn modal_request,
    std::shared_ptr<SkillStore> skills, SubagentToolFn subagent_tool,
    std::function<void(std::string)> on_finish)
    : state_(&state)
    , post_(std::move(post))
    , modal_request_(std::move(modal_request))
    , skills_(std::move(skills))
    , subagent_tool_(std::move(subagent_tool))
    , on_finish_(std::move(on_finish))
    , stream_fn_(std::move(stream_fn))
    , has_stream_override_(static_cast<bool>(stream_fn_))
    , tools_(std::move(tools))
{
    for (Tool& tool : tools_) {
        if (tool.spec.name != "skill") {
            continue;
        }
        tool.run = [this](const Json::Value& args) {
            const auto skill
                = resolve_skill(state_->environment->skills(), args);
            if (!skill) {
                return ToolOutput { ToolOutput::Kind::ERROR,
                    "skill: unknown or unavailable skill" };
            }
            if (skill_policy(state_->providers->config(), *skill)
                == SkillPolicy::DENY) {
                return ToolOutput { ToolOutput::Kind::ERROR,
                    "skill: access denied by configuration" };
            }
            const SkillRead read = read_skill(*skill);
            if (read.kind == SkillRead::Kind::READ_FAILED) {
                return ToolOutput { ToolOutput::Kind::ERROR,
                    "skill: cannot read instructions" };
            }
            if (read.kind == SkillRead::Kind::TOO_LARGE) {
                return ToolOutput { ToolOutput::Kind::ERROR,
                    "skill: instructions exceed 128 KiB" };
            }
            return ToolOutput { ToolOutput::Kind::OUTPUT, read.body };
        };
        break;
    }
    specs_plan_ = plan_tool_specs(tools_);
    specs_all_  = tool_specs(tools_);

    if (!stream_fn_) {
        stream_fn_ = [this](const ChatRequest& req, const StreamCallback& cb) {
            const auto selection = state_->providers->active_selection();
            const Route route
                = selection.has_value() ? selection->route : Route { };
            return stream(route, req, cb, &retry_after_secs_);
        };
    }
}

TurnRunner::~TurnRunner()
{
    alive_.store(false);
    worker_.reset();
}

void TurnRunner::spawn(std::vector<Message> history, TurnSettings settings)
{
    state_->session->append_assistant(
        settings.model, settings.reasoning_effort);
    worker_.emplace([this, history = std::move(history),
                        settings = std::move(settings)]() mutable {
        _drive(std::move(history), std::move(settings));
    });
}

void TurnRunner::clear()
{
    allowed_tools_.clear();
    stream_events_.clear();
}

void TurnRunner::stop() { alive_.store(false); }

void TurnRunner::set_on_finish(std::function<void(std::string)> on_finish)
{
    on_finish_ = std::move(on_finish);
}

void TurnRunner::set_subagent_tool(SubagentToolFn subagent_tool)
{
    subagent_tool_ = std::move(subagent_tool);
}

void TurnRunner::_post(std::function<void()> f)
{
    if (alive_.load()) {
        post_(std::move(f));
    }
}

bool TurnRunner::_compact_history(std::vector<Message>& history,
    const TurnSettings& settings, std::uint64_t prompt_tokens)
{
    const ModelPricing pricing = get_pricing(settings.model);
    if (pricing.context_limit == 0 || prompt_tokens == 0
        || prompt_tokens * 100 < pricing.context_limit * COMPACTION_PERCENT
        || history.size() < 4) {
        return true;
    }

    std::size_t tail = history.size();
    while (tail > 1 && history[tail - 1].type != Message::Type::USER) {
        --tail;
    }
    if (tail <= 1) {
        return true;
    }
    --tail;

    const auto [event_id, prefix_size] = state_->session->begin_compaction();
    ChatRequest request;
    request.model       = settings.model;
    request.temperature = 0.2;
    request.interrupted = [session = state_->session] {
        return session->interrupt_requested();
    };
    request.messages = {
        { Message::Type::SYSTEM,
            "Summarize this coding-agent session for continuation. Preserve "
            "the user's requirements, decisions, files changed, commands and "
            "test results, unresolved problems, and the exact current task. "
            "Be concise and do not continue the task." },
        { Message::Type::USER, compaction_transcript(history, tail) },
    };

    std::string summary;
    std::string error;
    const StreamCallback callback = [&](const StreamEvent& event) {
        if (event.kind == StreamEvent::Kind::CONTENT_DELTA) {
            summary += event.text;
        } else if (event.kind == StreamEvent::Kind::ERROR) {
            error = event.text;
        }
    };

    Status status;
    if (has_stream_override_) {
        status = stream_fn_(request, callback);
    } else {
        status = stream(settings.route, request, callback, nullptr);
    }
    const bool success = status == Status::OK && error.empty()
        && !summary.empty() && !state_->session->interrupt_requested();
    state_->session->finish_compaction(event_id, summary, prefix_size, success);
    if (!success) {
        return !state_->session->interrupt_requested();
    }

    std::vector<Message> recent(history.begin() + tail, history.end());
    history.erase(history.begin() + 1, history.end());
    history.push_back({ Message::Type::USER,
        "<session-summary>\n" + summary + "\n</session-summary>" });
    history.insert(history.end(), std::make_move_iterator(recent.begin()),
        std::make_move_iterator(recent.end()));
    return true;
}

void TurnRunner::_drive(
    std::vector<Message> history, TurnSettings settings)
{
    int retries                 = 0;
    std::uint64_t prompt_tokens = state_->session->last().prompt;
    bool compaction_attempted   = false;
    for (;;) {
        const ModelPricing pricing = get_pricing(settings.model);
        const bool should_compact  = !compaction_attempted
            && pricing.context_limit > 0 && prompt_tokens > 0
            && prompt_tokens * 100 >= pricing.context_limit * COMPACTION_PERCENT
            && history.size() >= 4;
        if (should_compact) {
            compaction_attempted = true;
        }
        if (should_compact
            && !_compact_history(history, settings, prompt_tokens)) {
            _post([this] { on_finish_(""); });
            return;
        }
        prompt_tokens = 0;
        ChatRequest req;
        req.model    = settings.model;
        req.messages = history;
        req.tools
            = settings.mode == Session::Mode::PLAN ? specs_plan_ : specs_all_;
        req.interrupted = [session = state_->session] {
            return session->interrupt_requested();
        };
        state_->session->reset_reasoning();
        stream_events_.clear();
        std::string text_buffer;
        std::string error_msg;
        Status error_status                 = Status::OK;
        bool saw_stream                     = false;
        std::uint64_t request_prompt_tokens = 0;

        StreamCallback cb = [this, model = req.model, &text_buffer, &error_msg,
                                &error_status, &saw_stream,
                                &request_prompt_tokens](const StreamEvent& ev) {
            if (ev.kind == StreamEvent::Kind::ERROR) {
                error_status = ev.error;
                error_msg    = ev.text;
                return;
            }
            if (ev.kind == StreamEvent::Kind::TOOL_CALL
                || ev.kind == StreamEvent::Kind::QUESTION) {
                stream_events_.push_back(ev);
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA
                || ev.kind == StreamEvent::Kind::CONNECTED) {
                saw_stream = true;
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA) {
                text_buffer += ev.text;
            }
            if (ev.kind == StreamEvent::Kind::USAGE) {
                request_prompt_tokens = ev.usage.prompt;
            }
            const ModelPricing pricing = ev.kind == StreamEvent::Kind::USAGE
                ? get_pricing(model)
                : ModelPricing { };
            _post([this, ev, pricing] { state_->session->apply(ev, pricing); });
        };

        const StreamFn& fn = stream_fn_;
        retry_after_secs_  = 0;
        Status st;
        ApiStandard active_dialect = ApiStandard::OPENAI;
        std::string current_model;
        std::string current_effort;
        if (has_stream_override_) {
            req.reasoning_effort = settings.reasoning_effort == "off"
                ? std::nullopt
                : std::optional<std::string>(
                      to_wire_effort(settings.reasoning_effort));
            current_model        = req.model;
            current_effort       = settings.reasoning_effort;
            state_->session->set_last_assistant_metadata(
                current_model, current_effort);
            st = fn(req, cb);
        } else {
            Route route = settings.route;
            apply_reasoning(req, route.dialect, settings.reasoning_effort,
                *state_->providers);
            active_dialect = route.dialect;
            current_model  = req.model;
            current_effort = req.reasoning_effort.has_value()
                    || req.thinking_budget.has_value()
                ? settings.reasoning_effort
                : "off";
            state_->session->set_last_assistant_metadata(
                current_model, current_effort);
            st = stream(route, req, cb, &retry_after_secs_);
            const Status attempt
                = error_status != Status::OK ? error_status : st;
            if (attempt == Status::API_ERROR && !saw_stream
                && route.dialect == ApiStandard::OPENAI) {
                Route alt;
                alt = state_->providers->route_for(
                    settings.connection_id, ApiStandard::ANTHROPIC);
                const bool has_alt = !alt.endpoint.empty();
                if (has_alt && alt.endpoint != route.endpoint) {
                    retry_after_secs_ = 0;
                    error_status      = Status::OK;
                    error_msg.clear();
                    apply_reasoning(req, ApiStandard::ANTHROPIC,
                        settings.reasoning_effort, *state_->providers);
                    st             = stream(alt, req, cb, &retry_after_secs_);
                    active_dialect = ApiStandard::ANTHROPIC;
                    current_effort = req.thinking_budget.has_value()
                        ? settings.reasoning_effort
                        : "off";
                    state_->session->set_last_assistant_metadata(
                        current_model, current_effort);
                    const Status retried
                        = error_status != Status::OK ? error_status : st;
                    if (retried == Status::OK) {
                        state_->providers->remember_dialect(
                            settings.connection_id, req.model,
                            ApiStandard::ANTHROPIC);
                    }
                }
            }
        }

        if (state_->session->interrupt_requested()) {
            _post([this] { on_finish_(""); });
            return;
        }

        const Status fail = error_status != Status::OK ? error_status : st;
        if (fail == Status::RATE_LIMITED && retries < 2) {
            ++retries;
            int wait = retry_after_secs_;
            if (wait <= 0) {
                wait = retries == 1 ? 2 : 5;
            }
            wait = std::clamp(wait, 1, 30);
            _post([this, wait] { state_->session->mark_retry(wait); });
            using namespace std::chrono_literals;
            const auto deadline
                = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!alive_.load() || state_->session->interrupt_requested()) {
                    _post([this] { on_finish_(""); });
                    return;
                }
                std::this_thread::sleep_for(50ms);
            }
            continue;
        }
        if (fail != Status::OK) {
            _post([this, fail, msg = error_msg] {
                state_->session->clear_error();
                std::string error = error_text(fail);
                if (!msg.empty()) {
                    if (error.ends_with('.')) {
                        error.pop_back();
                    }
                    error += ": " + msg;
                    error = ensure_sentence_end(std::move(error));
                }
                on_finish_(std::move(error));
            });
            return;
        }
        retries       = 0;
        prompt_tokens = request_prompt_tokens;

        if (!alive_.load()) {
            return;
        }

        std::string reply_buffer;
        const size_t history_before = history.size();
        _drain_pending_asks(history, reply_buffer, text_buffer, active_dialect);
        if (!alive_.load()) {
            return;
        }
        if (state_->session->interrupt_requested()) {
            _post([this] { on_finish_(""); });
            return;
        }

        if (history.size() == history_before) {
            _post([this] { on_finish_(""); });
            return;
        }
        _post([this, settings] {
            state_->session->append_assistant(
                settings.model, settings.reasoning_effort);
        });
    }
}

void TurnRunner::_drain_pending_asks(std::vector<Message>& history,
    std::string& reply_buffer, const std::string& assistant_text,
    ApiStandard dialect)
{
    struct Ask {
        ModalPayload payload;
        std::future<ModalResult> future;
        std::optional<ToolCallRequest> tool_req;
    };
    std::vector<Ask> asks;
    std::vector<Message> tool_msgs;
    bool had_tool_calls = false;

    for (const auto& ev : stream_events_) {
        if (ev.kind == StreamEvent::Kind::TOOL_CALL) {
            had_tool_calls = true;
            if (ev.tool_call.name == "ask") {
                auto form = parse_ask_args(ev.tool_call.args);
                if (!form) {
                    const ToolCallRequest req = ev.tool_call;
                    _post([this, req] {
                        state_->session->fill_tool_result(req,
                            ToolCall::Result { ToolCall::Result::Kind::ERROR,
                                "ask: expected a non-empty 'questions' "
                                "array" });
                    });
                    tool_msgs.push_back({ Message::Type::TOOL,
                        "ask: expected a non-empty 'questions' array", { },
                        req.id });
                    continue;
                }
                Ask ask;
                ask.payload  = *form;
                ask.future   = modal_request_(*form);
                ask.tool_req = ev.tool_call;
                asks.push_back(std::move(ask));
                continue;
            }
            if (ev.tool_call.name == "todo") {
                const std::optional<TodoList> list
                    = parse_todo_args(parse_json(ev.tool_call.args));
                const ToolCallRequest req = ev.tool_call;
                if (!list.has_value()) {
                    const std::string msg
                        = "todo: expected a 'todos' array of {content, status} "
                          "objects";
                    _post([this, req, msg] {
                        state_->session->fill_tool_result(req,
                            ToolCall::Result {
                                ToolCall::Result::Kind::ERROR, msg });
                    });
                    tool_msgs.push_back(
                        { Message::Type::TOOL, msg, { }, req.id });
                    continue;
                }
                const std::string text = todo_summary(*list);
                _post([this, req, todo = *list, text] {
                    state_->session->set_todo(todo);
                    state_->session->fill_tool_result(req,
                        ToolCall::Result {
                            ToolCall::Result::Kind::OUTPUT, text });
                });
                tool_msgs.push_back({ Message::Type::TOOL, text, { }, req.id });
                continue;
            }
            if (ev.tool_call.name == "subagent") {
                subagent_tool_(ev.tool_call, tool_msgs);
                continue;
            }
            const Tool* tool    = find_tool(tools_, ev.tool_call.name);
            bool needs_approval = tool != nullptr
                && tool->safety == ToolSafety::MUTATING
                && allowed_tools_.count(ev.tool_call.name) == 0;
            if (ev.tool_call.name == "skill") {
                const auto skill
                    = resolve_skill(state_->environment->skills(),
                        parse_json(ev.tool_call.args));
                needs_approval = skill.has_value()
                    && skill_policy(state_->providers->config(), *skill)
                        == SkillPolicy::ASK
                    && !skills_->is_loaded(skill->path);
            }
            ToolCallRequest approval_request = ev.tool_call;
            const bool path_scoped           = ev.tool_call.name == "read"
                || ev.tool_call.name == "list" || ev.tool_call.name == "edit"
                || ev.tool_call.name == "write";
            if (path_scoped && allowed_tools_.count(ev.tool_call.name) == 0) {
                const ProjectTarget target = classify_project_target(
                    ev.tool_call.name, ev.tool_call.args);
                if (target == ProjectTarget::OUTSIDE) {
                    needs_approval = true;
                    approval_request.approval_reason
                        = ToolCallRequest::ApprovalReason::OUTSIDE_WORKSPACE;
                } else {
                    needs_approval = false;
                }
            }
            if (!needs_approval) {
                _run_tool(ev.tool_call, tool_msgs);
                continue;
            }
            asks.push_back(Ask { .payload = approval_request,
                .future                   = modal_request_(approval_request),
                .tool_req                 = std::nullopt });
        } else if (ev.kind == StreamEvent::Kind::QUESTION) {
            asks.push_back(Ask { .payload = ev.question,
                .future                   = modal_request_(ev.question),
                .tool_req                 = std::nullopt });
        }
    }

    for (auto& ask : asks) {
        if (!alive_.load()) {
            return;
        }
        const ModalResult res = ask.future.get();
        if (const auto* req = std::get_if<ToolCallRequest>(&ask.payload)) {
            _apply_tool_result(*req, res, tool_msgs);
        } else if (ask.tool_req.has_value()) {
            _apply_ask_result(*ask.tool_req, res, tool_msgs);
        } else {
            _apply_question_result(res, reply_buffer);
        }
    }

    if (!had_tool_calls && asks.empty()) {
        return;
    }

    Message assistant { Message::Type::ASSISTANT, assistant_text };
    if (dialect == ApiStandard::ANTHROPIC) {
        if (const std::optional<AssistantTurn> a
            = state_->session->last_assistant()) {
            if (!a->reasoning.empty()) {
                assistant.thinking.push_back(
                    { a->reasoning, a->reasoning_signature });
            }
        }
    }
    for (const auto& ev : stream_events_) {
        if (ev.kind == StreamEvent::Kind::TOOL_CALL) {
            assistant.tool_calls.push_back(ToolCallEntry {
                ev.tool_call.id, ev.tool_call.name, ev.tool_call.args });
        }
    }
    if (!assistant.tool_calls.empty() || !assistant.content.empty()) {
        history.push_back(std::move(assistant));
    }
    for (auto& m : tool_msgs) {
        history.push_back(std::move(m));
    }
    if (!reply_buffer.empty()) {
        history.push_back({ Message::Type::USER, reply_buffer });
    }
}

void TurnRunner::_apply_tool_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs)
{
    const auto* verdict = std::get_if<ToolVerdict>(&res);
    if (verdict == nullptr) {
        _post([this, req] {
            state_->session->fill_tool_result(
                req, ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(""), { }, req.id });
        return;
    }
    if (verdict->decision == ToolDecision::REJECT) {
        std::string reason = verdict->reason;
        _post([this, req, reason] {
            state_->session->fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::REJECT, reason });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(reason), { }, req.id });
        return;
    }
    if (verdict->decision == ToolDecision::ACCEPT_ALWAYS) {
        const Tool* tool = find_tool(tools_, req.name);
        if (tool == nullptr || tool->persistent) {
            allowed_tools_.insert(req.name);
        }
    }
    _run_tool(req, tool_msgs);
}

void TurnRunner::_apply_question_result(
    const ModalResult& res, std::string& reply_buffer)
{
    const auto* answer = std::get_if<ModalAnswer>(&res);
    if (answer == nullptr) {
        return;
    }
    ModalAnswer copy = *answer;
    reply_buffer += modal_answer_markdown(copy);
    _post([this, copy = std::move(copy)] {
        state_->session->append_item(std::move(copy));
    });
}

void TurnRunner::_apply_ask_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs)
{
    const auto* answer = std::get_if<ModalAnswer>(&res);
    if (answer == nullptr) {
        _post([this, req] {
            state_->session->fill_tool_result(
                req, ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(""), { }, req.id });
        return;
    }
    ModalAnswer copy       = *answer;
    const std::string text = ask_answer_markdown(copy);
    _post([this, req, text] {
        state_->session->fill_tool_result(
            req, ToolCall::Result { ToolCall::Result::Kind::OUTPUT, text });
    });
    tool_msgs.push_back({ Message::Type::TOOL, text, { }, req.id });
}

void TurnRunner::_run_tool(
    const ToolCallRequest& req, std::vector<Message>& tool_msgs)
{
    const ToolOutput out = dispatch_tool(tools_, req);
    if (req.name == "skill" && out.kind == ToolOutput::Kind::OUTPUT) {
        if (const auto skill
            = resolve_skill(state_->environment->skills(),
                parse_json(req.args))) {
            skills_->record_tool_load(skill->path, out.text);
        }
    }
    const auto kind = out.kind == ToolOutput::Kind::OUTPUT
        ? ToolCall::Result::Kind::OUTPUT
        : ToolCall::Result::Kind::ERROR;
    _post([this, req, kind, out] {
        ToolCall::Result result { kind, out.text };
        result.diff         = out.diff;
        result.shell_status = out.shell_status;
        state_->session->fill_tool_result(req, std::move(result));
    });
    tool_msgs.push_back({ Message::Type::TOOL,
        append_shell_status(std::move(out.text), out.shell_status), { },
        req.id });
}

} // namespace ursa
