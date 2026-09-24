#include "turn/turn_runner.h"
#include "common/types.h"
#include "common/util.h"
#include "conversation/format.h"
#include "permissions/evaluator.h"
#include "providers/pricing.h"
#include "providers/store.h"
#include "tools/skills.h"
#include "turn/prompt.h"
#include "workspace/attachments.h"

#include <algorithm>
#include <chrono>
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

    constexpr std::size_t TURN_LINE_CAP    = 40;
    constexpr std::size_t TOOL_LINE_CAP    = 8;
    constexpr std::string_view TOOL_PREFIX = "TOOL: ";

    std::string capped(std::string_view text, std::size_t max_lines)
    {
        const std::string body = take_lines(text, max_lines);
        return count_lines(text) > max_lines ? body + "\n…" : body;
    }

    std::string item_transcript(const ConversationItem& item)
    {
        std::string out;
        if (const auto* user = std::get_if<UserTurn>(&item)) {
            out += "\nUSER:\n";
            out += capped(
                message_with_attachments(user->text, user->attachments),
                TURN_LINE_CAP);
        } else if (const auto* assistant = std::get_if<AssistantTurn>(&item)) {
            out += "\nASSISTANT:\n";
            out += capped(assistant->markdown, TURN_LINE_CAP);
        } else if (const auto* call = std::get_if<ToolCall>(&item)) {
            out += "\n";
            out += TOOL_PREFIX;
            out += call->name;
            out += "\n";
            out += capped(call->args, TOOL_LINE_CAP);
            if (call->result) {
                out += "\nRESULT:\n";
                out += capped(tool_result_text(*call), TOOL_LINE_CAP);
            }
        }
        return out;
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

std::string conversation_transcript(
    const std::string& compacted_summary, const SessionSnapshot& snapshot)
{
    std::string out;
    if (!compacted_summary.empty()) {
        out += "\nUSER:\n<session-summary>\n";
        out += compacted_summary;
        out += "\n";
    }
    for (const ConversationItem& item : snapshot.items) {
        out += item_transcript(item);
    }
    return out;
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
    std::shared_ptr<SkillStore> skills,
    std::function<void(std::string)> on_finish)
    : _state(&state)
    , _post_fn(std::move(post))
    , _modal_request(std::move(modal_request))
    , _skills(std::move(skills))
    , _on_finish(std::move(on_finish))
    , _stream_fn(std::move(stream_fn))
    , _has_stream_override(static_cast<bool>(_stream_fn))
    , _tools(std::move(tools))
{
    _specs_all = tool_specs(_tools);

    if (!_stream_fn) {
        _stream_fn = [this](const ChatRequest& req, const StreamCallback& cb) {
            const auto selection = _state->providers->active_selection();
            const Route route
                = selection.has_value() ? selection->route : Route { };
            return stream(route, req, cb, &_retry_after_secs);
        };
    }
}

TurnRunner::~TurnRunner()
{
    _alive.store(false);
    _worker.reset();
}

void TurnRunner::spawn(std::vector<Message> history, TurnSettings settings)
{
    _blocked_permission.store(false);
    _state->session->append_assistant(
        settings.model, settings.reasoning_effort);
    _worker.emplace([this, history = std::move(history),
                        settings = std::move(settings)]() mutable {
        _drive(std::move(history), std::move(settings));
    });
}

void TurnRunner::clear()
{
    _blocked_permission.store(false);
    _stream_events.clear();
}

void TurnRunner::stop() { _alive.store(false); }

void TurnRunner::set_on_finish(std::function<void(std::string)> on_finish)
{
    _on_finish = std::move(on_finish);
}

void TurnRunner::_post(std::function<void()> f)
{
    if (_alive.load()) {
        _post_fn(std::move(f));
    }
}

Status TurnRunner::run_stream(
    const ChatRequest& req, const Route& route, const StreamCallback& cb) const
{
    return _has_stream_override ? _stream_fn(req, cb)
                                : stream(route, req, cb, nullptr);
}

bool TurnRunner::_compact_history(std::vector<Message>& history,
    const TurnSettings& settings, std::uint64_t prompt_tokens)
{
    const ModelPricing pricing = _state->providers->pricing_for(settings.model);
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

    const auto [event_id, prefix_size] = _state->session->begin_compaction();
    ChatRequest request;
    request.model       = settings.model;
    request.temperature = 0.2;
    request.interrupted = [session = _state->session] {
        return session->interrupt_requested();
    };
    request.messages = {
        { Message::Type::SYSTEM, _state->prompts->compaction() },
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
        && !summary.empty() && !_state->session->interrupt_requested();
    _state->session->finish_compaction(event_id, summary, prefix_size, success);
    if (!success) {
        return !_state->session->interrupt_requested();
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
    if (!_has_stream_override) {
        settings.route
            = _state->providers->authenticated_route_for(settings.connection_id,
                settings.dialect, _state->session->session_id());
        settings.dialect = settings.route.dialect;
    }
    int retries                 = 0;
    std::uint64_t prompt_tokens = _state->session->last().prompt;
    bool compaction_attempted   = false;
    for (;;) {
        const ModelPricing pricing
            = _state->providers->pricing_for(settings.model);
        const bool should_compact = !compaction_attempted
            && compaction_due(pricing, prompt_tokens, history.size());
        if (should_compact) {
            compaction_attempted = true;
        }
        if (should_compact
            && !_compact_history(history, settings, prompt_tokens)) {
            _post([this] { _on_finish(""); });
            return;
        }
        prompt_tokens = 0;
        ChatRequest req;
        req.model       = settings.model;
        req.messages    = std::move(history);
        req.tools       = _specs_all;
        req.interrupted = [session = _state->session] {
            return session->interrupt_requested();
        };
        _state->session->reset_reasoning();
        _stream_events.clear();
        std::string text_buffer;
        std::string error_msg;
        Status error_status                 = Status::OK;
        bool saw_stream                     = false;
        bool received_data                  = false;
        std::uint64_t request_prompt_tokens = 0;
        StreamUpdateBuffer stream_updates(
            [this](std::function<void()> f) { _post(std::move(f)); },
            _state->session);

        StreamCallback cb = [this, model = req.model, &text_buffer, &error_msg,
                                &error_status, &saw_stream, &received_data,
                                &request_prompt_tokens,
                                &stream_updates](const StreamEvent& ev) {
            if (ev.kind == StreamEvent::Kind::ERROR) {
                error_status = ev.error;
                error_msg    = ev.text;
                return;
            }
            if (ev.kind == StreamEvent::Kind::TOOL_CALL
                || ev.kind == StreamEvent::Kind::QUESTION) {
                _stream_events.push_back(ev);
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA
                || ev.kind == StreamEvent::Kind::CONNECTED) {
                saw_stream = true;
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA
                || ev.kind == StreamEvent::Kind::REASONING
                || ev.kind == StreamEvent::Kind::TOOL_CALL_START
                || ev.kind == StreamEvent::Kind::TOOL_CALL
                || ev.kind == StreamEvent::Kind::USAGE) {
                received_data = true;
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA) {
                text_buffer += ev.text;
            }
            if (ev.kind == StreamEvent::Kind::USAGE) {
                request_prompt_tokens = ev.usage.prompt;
            }
            const ModelPricing pricing = ev.kind == StreamEvent::Kind::USAGE
                ? _state->providers->pricing_for(model)
                : ModelPricing { };
            stream_updates.push(ev, pricing);
        };

        const StreamFn& fn = _stream_fn;
        _retry_after_secs  = 0;
        Status st;
        ApiStandard active_dialect = ApiStandard::OPENAI;
        std::string current_model;
        std::string current_effort;
        if (_has_stream_override) {
            req.reasoning_effort = settings.reasoning_effort == "off"
                ? std::nullopt
                : std::optional<std::string>(
                      to_wire_effort(settings.reasoning_effort));
            current_model        = req.model;
            current_effort       = settings.reasoning_effort;
            _state->session->set_last_assistant_metadata(
                current_model, current_effort);
            st = fn(req, cb);
        } else {
            Route route          = settings.route;
            current_model        = req.model;
            const auto try_route = [&](const Route& candidate) {
                _retry_after_secs = 0;
                error_status      = Status::OK;
                error_msg.clear();
                apply_reasoning(req, candidate.dialect,
                    settings.reasoning_effort, *_state->providers);
                active_dialect = candidate.dialect;
                current_effort = req.reasoning_effort.has_value()
                        || req.thinking_budget.has_value()
                    ? settings.reasoning_effort
                    : "off";
                _state->session->set_last_assistant_metadata(
                    current_model, current_effort);
                return stream(candidate, req, cb, &_retry_after_secs);
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
                Route alternative = _state->providers->route_for(
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
                    _state->providers->remember_dialect(
                        settings.connection_id, req.model, dialect);
                    settings.route   = route;
                    settings.dialect = route.dialect;
                    break;
                }
            }
        }
        stream_updates.finish();
        history = std::move(req.messages);

        if (_state->session->interrupt_requested()) {
            _post([this] { _on_finish(""); });
            return;
        }

        const Status fail = error_status != Status::OK ? error_status : st;
        // A stalled connection or a transient server failure before any
        // data can be retried safely; once content has arrived a retry
        // would duplicate visible output.
        const bool retryable_stall = !received_data
            && (fail == Status::TIMEOUT || fail == Status::NETWORK_ERROR);
        const bool retryable_server
            = !received_data && fail == Status::SERVER_ERROR;
        if ((fail == Status::RATE_LIMITED || retryable_stall
                || retryable_server)
            && retries < 2) {
            ++retries;
            int wait = _retry_after_secs;
            if (wait <= 0) {
                wait = retries == 1 ? 2 : 5;
            }
            wait = std::clamp(wait, 1, 30);
            _post([this, wait, stalled = retryable_stall] {
                _state->session->mark_retry(wait, stalled);
            });
            using namespace std::chrono_literals;
            const auto deadline
                = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!_alive.load() || _state->session->interrupt_requested()) {
                    _post([this] { _on_finish(""); });
                    return;
                }
                std::this_thread::sleep_for(50ms);
            }
            continue;
        }
        if (fail != Status::OK) {
            _post([this, fail, msg = error_msg] {
                _state->session->clear_error();
                std::string error = error_text(fail);
                if (!msg.empty()) {
                    if (error.ends_with('.')) {
                        error.pop_back();
                    }
                    error += ": " + msg;
                    error = ensure_sentence_end(std::move(error));
                }
                _on_finish(std::move(error));
            });
            return;
        }
        retries       = 0;
        prompt_tokens = request_prompt_tokens;

        if (!_alive.load()) {
            return;
        }

        std::string reply_buffer;
        const size_t history_before = history.size();
        _drain_pending_asks(
            history, reply_buffer, text_buffer, active_dialect, settings.mode);
        if (!_alive.load()) {
            return;
        }
        if (_state->session->interrupt_requested()) {
            _post([this] { _on_finish(""); });
            return;
        }

        if (history.size() == history_before) {
            _post([this] { _on_finish(""); });
            return;
        }
        _post([this, settings] {
            _state->session->append_assistant(
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

    for (const auto& ev : _stream_events) {
        if (!_alive.load()) {
            return;
        }
        if (ev.kind == StreamEvent::Kind::QUESTION) {
            if ((_state->runtime_flags & RuntimeFlag::ATTENDED)
                == RuntimeFlag::NONE) {
                _blocked_permission.store(true);
                continue;
            }
            const ModalResult res = _modal_request(ev.question).get();
            _apply_question_result(res, reply_buffer);
            if (_state->session->interrupt_requested()) {
                return;
            }
            continue;
        }
        if (ev.kind != StreamEvent::Kind::TOOL_CALL) {
            continue;
        }

        had_tool_calls                  = true;
        const ToolCallRequest& original = ev.tool_call;
        if (find_tool(_tools, original.name) == nullptr) {
            const std::string error = "unknown tool: " + original.name;
            _finish_tool(
                original, ToolCall::Result::Kind::ERROR, error, tool_msgs);
            continue;
        }

        const PermissionEvaluation evaluation = evaluate_tool_request(original,
            make_permission_context(
                *_state->environment, *_state->permissions, mode),
            _state->providers->config(), _state->environment->skills(),
            *_skills);
        if (evaluation.decision.kind == PermissionDecision::Kind::REJECT) {
            _reject_tool(original, evaluation.decision.reason, tool_msgs);
            continue;
        }
        if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT
            || (_state->runtime_flags & RuntimeFlag::SKIP_PERMISSIONS)
                != RuntimeFlag::NONE) {
            _run_tool(evaluation, mode, tool_msgs);
            continue;
        }
        if ((_state->runtime_flags & RuntimeFlag::ATTENDED)
            == RuntimeFlag::NONE) {
            _blocked_permission.store(true);
            _reject_tool(original,
                "permission is unavailable in unattended mode: "
                    + evaluation.decision.reason,
                tool_msgs);
            continue;
        }

        // The roster gate only asks for skills, which carry a prompt.
        const ModalResult res = _modal_request(*evaluation.prompt).get();
        _apply_tool_result(evaluation, res, mode, tool_msgs);
        if (_state->session->interrupt_requested()) {
            return;
        }
    }

    if (!had_tool_calls && reply_buffer.empty()) {
        return;
    }

    const std::optional<AssistantTurn> reasoning_source
        = _state->session->last_assistant();
    Message assistant = assistant_message(assistant_text,
        reasoning_source.has_value() ? &*reasoning_source : nullptr, dialect);
    for (const auto& ev : _stream_events) {
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
            || !_state->permissions->install(evaluation.session_grants)) {
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
        _state->session->append_item(std::move(copy));
    });
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
        _state->session->fill_tool_result(
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
        make_permission_context(
            *_state->environment, *_state->permissions, mode),
        _state->providers->config(), _state->environment->skills(), *_skills);
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
    ToolOutput out             = dispatch_tool(_tools, req);
    if (out.blocked_permission) {
        _blocked_permission.store(true);
    }
    const auto kind          = out.kind == ToolOutput::Kind::OUTPUT
        ? ToolCall::Result::Kind::OUTPUT
        : ToolCall::Result::Kind::ERROR;
    std::string history_text = format_lua_result(out.text, out.return_value);
    _post([this, req, kind, out = std::move(out)]() mutable {
        ToolCall::Result result { kind, std::move(out.text) };
        result.return_value = std::move(out.return_value);
        result.diffs        = std::move(out.diffs);
        result.dispatch_log = std::move(out.dispatch_log);
        _state->session->fill_tool_result(req, std::move(result));
    });
    tool_msgs.push_back(
        { Message::Type::TOOL, std::move(history_text), { }, req.id });
}

} // namespace imza
