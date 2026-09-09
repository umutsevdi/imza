#include "turn/turn_runner.h"
#include "common/types.h"
#include "common/util.h"
#include "conversation/format.h"
#include "network/json_io.h"
#include "permissions/evaluator.h"
#include "providers/pricing.h"
#include "providers/store.h"
#include "tools/skills.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace imza {

namespace {

    constexpr std::uint64_t COMPACTION_PERCENT = 80;

    bool tool_available_in_mode(std::string_view name, Session::Mode mode)
    {
        return mode == Session::Mode::BUILD
            || (name != "edit" && name != "write");
    }

    std::vector<ToolSpec> tool_specs_for_mode(
        const std::vector<ToolSpec>& specs, Session::Mode mode)
    {
        std::vector<ToolSpec> available;
        std::ranges::copy_if(
            specs, std::back_inserter(available), [mode](const ToolSpec& spec) {
                return tool_available_in_mode(spec.name, mode);
            });
        return available;
    }

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

    bool compaction_due(const ModelPricing& pricing,
        std::uint64_t prompt_tokens, std::size_t history_size)
    {
        return pricing.context_limit > 0 && prompt_tokens > 0
            && prompt_tokens * 100 >= pricing.context_limit * COMPACTION_PERCENT
            && history_size >= 4;
    }

} // namespace

TurnSettings make_turn_settings(
    const ProviderSelection& selection, Session::Mode mode)
{
    TurnSettings settings;
    settings.model            = selection.model;
    settings.reasoning_effort = to_config_effort(selection.reasoning_effort);
    settings.connection_id    = selection.connection_id;
    settings.route            = selection.route;
    settings.dialect          = selection.route.dialect;
    settings.mode             = mode;
    return settings;
}

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
        req.thinking_budget = configured == "low" ? 2000
            : configured == "high"                ? 16000
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
            const std::optional<std::filesystem::path> path
                = canonical_skill_path(*skill);
            if (!path || !args["path"].isString()
                || args["path"].asString() != path->string()) {
                return ToolOutput { ToolOutput::Kind::ERROR,
                    "skill: permission target changed before execution" };
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
    specs_all_ = tool_specs(tools_);

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
    blocked_permission_.store(false);
    state_->session->append_assistant(
        settings.model, settings.reasoning_effort);
    worker_.emplace([this, history = std::move(history),
                        settings = std::move(settings)]() mutable {
        _drive(std::move(history), std::move(settings));
    });
}

void TurnRunner::clear()
{
    blocked_permission_.store(false);
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

Status TurnRunner::run_stream(
    const ChatRequest& req, const Route& route, const StreamCallback& cb) const
{
    return has_stream_override_ ? stream_fn_(req, cb)
                                : stream(route, req, cb, nullptr);
}

bool TurnRunner::_compact_history(std::vector<Message>& history,
    const TurnSettings& settings, std::uint64_t prompt_tokens)
{
    const ModelPricing pricing = state_->providers->pricing_for(settings.model);
    if (!compaction_due(pricing, prompt_tokens, history.size())) {
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

    const Status status = run_stream(request, settings.route, callback);
    const bool success  = status == Status::OK && error.empty()
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

void TurnRunner::_drive(std::vector<Message> history, TurnSettings settings)
{
    if (!has_stream_override_) {
        settings.route = state_->providers->authenticated_route_for(
            settings.connection_id, settings.dialect);
        settings.dialect = settings.route.dialect;
    }
    int retries                 = 0;
    std::uint64_t prompt_tokens = state_->session->last().prompt;
    bool compaction_attempted   = false;
    for (;;) {
        const ModelPricing pricing
            = state_->providers->pricing_for(settings.model);
        const bool should_compact = !compaction_attempted
            && compaction_due(pricing, prompt_tokens, history.size());
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
        req.model       = settings.model;
        req.messages    = std::move(history);
        req.tools       = tool_specs_for_mode(specs_all_, settings.mode);
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
        StreamUpdateBuffer stream_updates(post_, state_->session);

        StreamCallback cb = [this, model = req.model, &text_buffer, &error_msg,
                                &error_status, &saw_stream,
                                &request_prompt_tokens,
                                &stream_updates](const StreamEvent& ev) {
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
                ? state_->providers->pricing_for(model)
                : ModelPricing { };
            stream_updates.push(ev, pricing);
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
            Route route          = settings.route;
            current_model        = req.model;
            const auto try_route = [&](const Route& candidate) {
                retry_after_secs_ = 0;
                error_status      = Status::OK;
                error_msg.clear();
                apply_reasoning(req, candidate.dialect,
                    settings.reasoning_effort, *state_->providers);
                active_dialect = candidate.dialect;
                current_effort = req.reasoning_effort.has_value()
                        || req.thinking_budget.has_value()
                    ? settings.reasoning_effort
                    : "off";
                state_->session->set_last_assistant_metadata(
                    current_model, current_effort);
                return stream(candidate, req, cb, &retry_after_secs_);
            };

            std::vector<std::string> attempted_endpoints { route.endpoint };
            st = try_route(route);
            std::vector<ApiStandard> alternatives;
            if (route.dialect == ApiStandard::OPENAI_RESPONSES) {
                alternatives = { ApiStandard::OPENAI, ApiStandard::ANTHROPIC };
            } else if (route.dialect == ApiStandard::OPENAI) {
                alternatives = route.auth == AuthType::ANTHROPIC
                    ? std::vector { ApiStandard::ANTHROPIC,
                          ApiStandard::OPENAI_RESPONSES }
                    : std::vector { ApiStandard::OPENAI_RESPONSES,
                          ApiStandard::ANTHROPIC };
            }
            for (const ApiStandard dialect : alternatives) {
                const Status attempt
                    = error_status != Status::OK ? error_status : st;
                if (attempt != Status::API_ERROR || saw_stream) {
                    break;
                }
                Route alternative = state_->providers->route_for(
                    settings.connection_id, dialect);
                if (alternative.endpoint.empty()
                    || std::find(attempted_endpoints.begin(),
                           attempted_endpoints.end(), alternative.endpoint)
                        != attempted_endpoints.end()) {
                    continue;
                }
                attempted_endpoints.push_back(alternative.endpoint);
                st    = try_route(alternative);
                route = std::move(alternative);
                const Status retried
                    = error_status != Status::OK ? error_status : st;
                if (retried == Status::OK) {
                    state_->providers->remember_dialect(
                        settings.connection_id, req.model, dialect);
                    settings.route   = route;
                    settings.dialect = route.dialect;
                    break;
                }
            }
        }
        stream_updates.finish();
        history = std::move(req.messages);

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
        _drain_pending_asks(
            history, reply_buffer, text_buffer, active_dialect, settings.mode);
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
    ApiStandard dialect, Session::Mode mode)
{
    std::vector<Message> tool_msgs;
    bool had_tool_calls = false;

    for (const auto& ev : stream_events_) {
        if (!alive_.load()) {
            return;
        }
        if (ev.kind == StreamEvent::Kind::QUESTION) {
            if ((state_->runtime_flags & RuntimeFlag::ATTENDED)
                == RuntimeFlag::NONE) {
                blocked_permission_.store(true);
                continue;
            }
            const ModalResult res = modal_request_(ev.question).get();
            _apply_question_result(res, reply_buffer);
            if (state_->session->interrupt_requested()) {
                return;
            }
            continue;
        }
        if (ev.kind != StreamEvent::Kind::TOOL_CALL) {
            continue;
        }

        had_tool_calls                  = true;
        const ToolCallRequest& original = ev.tool_call;
        if (!tool_available_in_mode(original.name, mode)) {
            _reject_tool(original,
                original.name + " is unavailable in Plan mode", tool_msgs);
            continue;
        }
        if (find_tool(tools_, original.name) == nullptr) {
            const std::string error = "unknown tool: " + original.name;
            _finish_tool(
                original, ToolCall::Result::Kind::ERROR, error, tool_msgs);
            continue;
        }

        const PermissionEvaluation evaluation = evaluate_tool_request(original,
            permission_context(
                *state_->environment, *state_->permissions, mode),
            state_->providers->config(), state_->environment->skills(),
            *skills_);
        if (evaluation.decision.kind == PermissionDecision::Kind::REJECT) {
            _reject_tool(original, evaluation.decision.reason, tool_msgs);
            continue;
        }
        if (original.name == "ask") {
            const auto form       = parse_ask_args(evaluation.request.args);
            const ModalResult res = modal_request_(*form).get();
            _apply_ask_result(original, res, tool_msgs);
            if (state_->session->interrupt_requested()) {
                return;
            }
            continue;
        }
        if (original.name == "todo") {
            const TodoList todo
                = *parse_todo_args(parse_json(evaluation.request.args));
            const std::string text = todo_summary(todo);
            _post([this, req = original, todo, text] {
                state_->session->set_todo(todo);
                state_->session->fill_tool_result(req,
                    ToolCall::Result { ToolCall::Result::Kind::OUTPUT, text });
            });
            tool_msgs.push_back(
                { Message::Type::TOOL, text, { }, original.id });
            continue;
        }
        if (original.name == "subagent") {
            subagent_tool_(evaluation.request, tool_msgs);
            continue;
        }
        if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT
            || (state_->runtime_flags & RuntimeFlag::SKIP_PERMISSIONS)
                != RuntimeFlag::NONE) {
            _run_tool(evaluation, mode, tool_msgs);
            continue;
        }
        if ((state_->runtime_flags & RuntimeFlag::ATTENDED)
            == RuntimeFlag::NONE) {
            blocked_permission_.store(true);
            _reject_tool(original,
                "permission is unavailable in unattended mode: "
                    + evaluation.decision.reason,
                tool_msgs);
            continue;
        }

        const ModalResult res = modal_request_(evaluation.request).get();
        _apply_tool_result(evaluation, res, mode, tool_msgs);
        if (state_->session->interrupt_requested()) {
            return;
        }
    }

    if (!had_tool_calls && reply_buffer.empty()) {
        return;
    }

    const std::optional<AssistantTurn> reasoning_source
        = state_->session->last_assistant();
    Message assistant = assistant_message(assistant_text,
        reasoning_source.has_value() ? &*reasoning_source : nullptr, dialect);
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

void TurnRunner::_apply_tool_result(const PermissionEvaluation& evaluation,
    const ModalResult& res, Session::Mode mode, std::vector<Message>& tool_msgs)
{
    const ToolCallRequest& req = evaluation.request;
    const auto* verdict        = std::get_if<ToolVerdict>(&res);
    if (verdict == nullptr) {
        _finish_tool(req, ToolCall::Result::Kind::CANCEL, "", denial_text(""),
            tool_msgs);
        return;
    }
    if (verdict->decision == ToolDecision::REJECT) {
        std::string reason = verdict->reason;
        _finish_tool(req, ToolCall::Result::Kind::REJECT, std::move(reason),
            denial_text(reason), tool_msgs);
        return;
    }
    if (verdict->decision == ToolDecision::ACCEPT_FOR_SESSION) {
        if (evaluation.session_grants.empty()
            || !state_->permissions->install(evaluation.session_grants)) {
            _reject_tool(req, "session approval is unavailable", tool_msgs);
            return;
        }
    }
    _run_tool(evaluation, mode, tool_msgs);
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
        _finish_tool(req, ToolCall::Result::Kind::CANCEL, "", denial_text(""),
            tool_msgs);
        return;
    }
    ModalAnswer copy       = *answer;
    const std::string text = ask_answer_markdown(copy);
    _finish_tool(req, ToolCall::Result::Kind::OUTPUT, text, tool_msgs);
}

void TurnRunner::_reject_tool(const ToolCallRequest& req, std::string reason,
    std::vector<Message>& tool_msgs)
{
    _finish_tool(req, ToolCall::Result::Kind::REJECT, std::move(reason),
        denial_text(reason), tool_msgs);
}

void TurnRunner::_finish_tool(const ToolCallRequest& req,
    ToolCall::Result::Kind kind, const std::string& history_text,
    std::vector<Message>& tool_msgs)
{
    _finish_tool(req, kind, history_text, history_text, tool_msgs);
}

void TurnRunner::_finish_tool(const ToolCallRequest& req,
    ToolCall::Result::Kind kind, std::string result_text,
    std::string history_text, std::vector<Message>& tool_msgs)
{
    _post([this, req, kind, result_text = std::move(result_text)]() mutable {
        state_->session->fill_tool_result(
            req, ToolCall::Result { kind, std::move(result_text) });
    });
    tool_msgs.push_back(
        { Message::Type::TOOL, std::move(history_text), { }, req.id });
}

void TurnRunner::_run_tool(const PermissionEvaluation& evaluation,
    Session::Mode mode, std::vector<Message>& tool_msgs)
{
    const PermissionEvaluation current = evaluate_tool_request(
        evaluation.request,
        permission_context(*state_->environment, *state_->permissions, mode),
        state_->providers->config(), state_->environment->skills(), *skills_);
    if (current.decision.kind == PermissionDecision::Kind::REJECT
        || current.request.args != evaluation.request.args) {
        const std::string reason
            = current.decision.kind == PermissionDecision::Kind::REJECT
            ? current.decision.reason
            : "permission target changed before execution";
        _reject_tool(evaluation.request, reason, tool_msgs);
        return;
    }

    const ToolCallRequest& req = current.request;
    ToolOutput out             = dispatch_tool(tools_, req);
    if (req.name == "skill" && out.kind == ToolOutput::Kind::OUTPUT) {
        if (const auto skill = resolve_skill(
                state_->environment->skills(), parse_json(req.args))) {
            const std::optional<std::filesystem::path> path
                = canonical_skill_path(*skill);
            skills_->record_tool_load(path.value_or(skill->path), out.text);
        }
    }
    const auto kind          = out.kind == ToolOutput::Kind::OUTPUT
        ? ToolCall::Result::Kind::OUTPUT
        : ToolCall::Result::Kind::ERROR;
    std::string history_text = append_shell_status(out.text, out.shell_status);
    _post([this, req, kind, out = std::move(out)]() mutable {
        ToolCall::Result result { kind, std::move(out.text) };
        result.diff         = std::move(out.diff);
        result.shell_status = std::move(out.shell_status);
        state_->session->fill_tool_result(req, std::move(result));
    });
    tool_msgs.push_back(
        { Message::Type::TOOL, std::move(history_text), { }, req.id });
}

} // namespace imza
