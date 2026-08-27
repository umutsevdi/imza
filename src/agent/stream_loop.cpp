#include "controller.h"

#include "format.h"
#include "pricing.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ursa {

namespace {

    bool auto_approved_in_project(const std::string& name,
        const std::string& args)
    {
        if (name != "edit" && name != "write") {
            return false;
        }
        const Json::Value parsed = parse_json(args);
        if (!parsed.isObject() || !parsed["file_path"].isString()) {
            return false;
        }
        std::error_code ec;
        const std::filesystem::path target = std::filesystem::weakly_canonical(
            std::filesystem::absolute(parsed["file_path"].asString()), ec);
        if (ec) {
            return false;
        }
        const std::filesystem::path root = std::filesystem::weakly_canonical(
            std::filesystem::current_path(ec), ec);
        if (ec) {
            return false;
        }
        const auto rel = target.lexically_relative(root);
        if (rel.empty()) {
            return true;
        }
        const std::string rel_str = rel.string();
        return rel_str.rfind("..", 0) != 0;
    }

    std::optional<QuestionForm> parse_ask_args(const std::string& args)
    {
        const Json::Value parsed = parse_json(args);
        if (!parsed.isObject() || !parsed["questions"].isArray()
            || parsed["questions"].empty()) {
            return std::nullopt;
        }
        QuestionForm form;
        for (const auto& q : parsed["questions"]) {
            QuestionCard card;
            if (!q.isObject() || !q["prompt"].isString()
                || q["prompt"].asString().empty()) {
                return std::nullopt;
            }
            card.prompt = q["prompt"].asString();
            if (q["options"].isArray()) {
                for (const auto& o : q["options"]) {
                    if (o.isString()) {
                        card.options.push_back(o.asString());
                    }
                }
            }
            if (q["multi"].isBool()) {
                card.multi = q["multi"].asBool();
            }
            if (q["free_text"].isBool()) {
                card.free_text = q["free_text"].asBool();
            }
            form.push_back(std::move(card));
        }
        if (form.empty()) {
            return std::nullopt;
        }
        return form;
    }

} // namespace

void Controller::_drive(std::vector<Message> history, StreamFn override,
    TurnSettings settings)
{
    int retries = 0;
    for (;;) {
        ChatRequest req;
        req.model = settings.model;
        req.messages = history;
        req.tools = settings.mode == Session::Mode::PLAN ? specs_plan_ : specs_all_;
        req.interrupted = [session = session_] {
            return session->interrupt_requested();
        };
        session_->reset_reasoning();
        stream_events_.clear();
        std::string text_buffer;
        std::string error_msg;
        Status error_status = Status::OK;
        bool saw_stream     = false;

        StreamCallback cb = [this, model = req.model, &text_buffer,
                                &error_msg, &error_status,
                                &saw_stream](const StreamEvent& ev) {
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
            const ModelPricing pricing = ev.kind == StreamEvent::Kind::USAGE
                ? get_pricing(model)
                : ModelPricing { };
            _post([this, ev, pricing] { session_->apply(ev, pricing); });
        };

        StreamFn fn       = override ? override : stream_fn_;
        retry_after_secs_ = 0;
        Status st;
        ApiStandard active_dialect = ApiStandard::OPENAI;
        std::string current_model;
        std::string current_effort;
        if (override) {
            req.reasoning_effort = settings.reasoning_effort == "default"
                ? std::optional<std::string>("medium")
                : settings.reasoning_effort == "off"
                ? std::nullopt
                : std::optional<std::string>(settings.reasoning_effort);
            current_model = req.model;
            current_effort = settings.reasoning_effort;
            session_->set_last_assistant_metadata(
                current_model, current_effort);
            st = fn(req, cb);
        } else {
            Route route = settings.route;
            _set_reasoning(req, route.dialect, settings.reasoning_effort);
            active_dialect = route.dialect;
            current_model = req.model;
            current_effort = req.reasoning_effort.has_value()
                    || req.thinking_budget.has_value()
                ? settings.reasoning_effort
                : "off";
            session_->set_last_assistant_metadata(
                current_model, current_effort);
            st = stream(
                get_provider(route), route, req, cb, &retry_after_secs_);
            const Status attempt
                = error_status != Status::OK ? error_status : st;
            if (attempt == Status::API_ERROR && !saw_stream
                && route.dialect == ApiStandard::OPENAI) {
                Route alt;
                alt = providers_.route_for(
                    settings.connection_id, ApiStandard::ANTHROPIC);
                const bool has_alt = !alt.endpoint.empty();
                if (has_alt && alt.endpoint != route.endpoint) {
                    retry_after_secs_ = 0;
                    error_status      = Status::OK;
                    error_msg.clear();
                    _set_reasoning(req, ApiStandard::ANTHROPIC,
                        settings.reasoning_effort);
                    st = stream(
                        get_provider(alt), alt, req, cb, &retry_after_secs_);
                    active_dialect = ApiStandard::ANTHROPIC;
                    current_effort = req.thinking_budget.has_value()
                        ? settings.reasoning_effort
                        : "off";
                    session_->set_last_assistant_metadata(
                        current_model, current_effort);
                    const Status retried
                        = error_status != Status::OK ? error_status : st;
                    if (retried == Status::OK) {
                        providers_.remember_dialect(settings.connection_id,
                            req.model, ApiStandard::ANTHROPIC);
                    }
                }
            }
        }

        if (session_->interrupt_requested()) {
            _post([this] { finish(""); });
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
            _post([this, wait] { session_->mark_retry(wait); });
            using namespace std::chrono_literals;
            const auto deadline
                = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!alive_.load() || session_->interrupt_requested()) {
                    _post([this] { finish(""); });
                    return;
                }
                std::this_thread::sleep_for(50ms);
            }
            continue;
        }
        if (fail != Status::OK) {
            _post([this, fail, msg = error_msg] {
                session_->clear_error();
                finish(msg.empty() ? error_text(fail)
                                   : error_text(fail) + ": " + msg);
            });
            return;
        }
        retries = 0;

        if (!alive_.load()) {
            return;
        }

        std::string reply_buffer;
        const size_t history_before = history.size();
        _drain_pending_asks(history, reply_buffer, text_buffer, active_dialect);
        if (!alive_.load()) {
            return;
        }
        if (session_->interrupt_requested()) {
            _post([this] { finish(""); });
            return;
        }

        if (history.size() == history_before) {
            _post([this] { finish(""); });
            return;
        }
        _post([this, settings] {
            session_->append_assistant(
                settings.model, settings.reasoning_effort);
        });
    }
}

void Controller::_drain_pending_asks(std::vector<Message>& history,
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
                        session_->fill_tool_result(req,
                            ToolCall::Result { ToolCall::Result::Kind::ERROR,
                                "ask: expected a non-empty 'questions' "
                                "array" });
                    });
                    tool_msgs.push_back({ Message::Type::TOOL,
                        "ask: expected a non-empty 'questions' array", { },
                        req.id });
                    continue;
                }
                auto promise = std::make_shared<std::promise<ModalResult>>();
                auto future  = promise->get_future();
                {
                    std::lock_guard lock(queue_mutex_);
                    queue_.push_back(
                        PendingModal { *form, std::move(promise) });
                }
                Ask ask;
                ask.payload  = *form;
                ask.future   = std::move(future);
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
                        session_->fill_tool_result(req,
                            ToolCall::Result {
                                ToolCall::Result::Kind::ERROR, msg });
                    });
                    tool_msgs.push_back(
                        { Message::Type::TOOL, msg, { }, req.id });
                    continue;
                }
                const std::string text = todo_summary(*list);
                _post([this, req, todo = *list, text] {
                    session_->set_todo(todo);
                    session_->fill_tool_result(req,
                        ToolCall::Result {
                            ToolCall::Result::Kind::OUTPUT, text });
                });
                tool_msgs.push_back({ Message::Type::TOOL, text, { }, req.id });
                continue;
            }
            const Tool* tool          = tools_.find(ev.tool_call.name);
            bool needs_approval = tool != nullptr
                && tool->safety == ToolSafety::MUTATING
                && allowed_tools_.count(ev.tool_call.name) == 0;
            if (needs_approval
                && auto_approved_in_project(
                    ev.tool_call.name, ev.tool_call.args)) {
                needs_approval = false;
            }
            if (!needs_approval) {
                _run_tool(ev.tool_call, tool_msgs);
                continue;
            }
            auto promise = std::make_shared<std::promise<ModalResult>>();
            auto future  = promise->get_future();
            {
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(
                    PendingModal { ev.tool_call, std::move(promise) });
            }
            asks.push_back(Ask { .payload = ev.tool_call,
                .future                   = std::move(future),
                .tool_req                 = std::nullopt });
        } else if (ev.kind == StreamEvent::Kind::QUESTION) {
            auto promise = std::make_shared<std::promise<ModalResult>>();
            auto future  = promise->get_future();
            {
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(
                    PendingModal { ev.question, std::move(promise) });
            }
            asks.push_back(Ask { .payload = ev.question,
                .future                   = std::move(future),
                .tool_req                 = std::nullopt });
        }
    }
    if (!asks.empty()) {
        _post([this] { _present_front(); });
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
        if (const std::optional<AssistantTurn> a = session_->last_assistant()) {
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

void Controller::_apply_tool_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs)
{
    const auto* verdict = std::get_if<ToolVerdict>(&res);
    if (verdict == nullptr) {
        _post([this, req] {
            session_->fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(""), { }, req.id });
        return;
    }
    if (verdict->decision == ToolDecision::REJECT) {
        std::string reason = verdict->reason;
        _post([this, req, reason] {
            session_->fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::REJECT, reason });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(reason), { }, req.id });
        return;
    }
    if (verdict->decision == ToolDecision::ACCEPT_ALWAYS) {
        const Tool* tool = tools_.find(req.name);
        if (tool == nullptr || tool->persistent) {
            allowed_tools_.insert(req.name);
        }
    }
    _run_tool(req, tool_msgs);
}

void Controller::_apply_question_result(
    const ModalResult& res, std::string& reply_buffer)
{
    const auto* answer = std::get_if<ModalAnswer>(&res);
    if (answer == nullptr) {
        return;
    }
    ModalAnswer copy = *answer;
    reply_buffer += modal_answer_markdown(copy);
    _post([this, copy = std::move(copy)] {
        session_->append_item(std::move(copy));
    });
}

void Controller::_apply_ask_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs)
{
    const auto* answer = std::get_if<ModalAnswer>(&res);
    if (answer == nullptr) {
        _post([this, req] {
            session_->fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(""), { }, req.id });
        return;
    }
    ModalAnswer copy       = *answer;
    const std::string text = ask_answer_markdown(copy);
    _post([this, req, text] {
        session_->fill_tool_result(req,
            ToolCall::Result { ToolCall::Result::Kind::OUTPUT, text });
    });
    tool_msgs.push_back({ Message::Type::TOOL, text, { }, req.id });
}

void Controller::_run_tool(
    const ToolCallRequest& req, std::vector<Message>& tool_msgs)
{
    const ToolOutput out = tools_.dispatch(req);
    const auto kind      = out.kind == ToolOutput::Kind::OUTPUT
        ? ToolCall::Result::Kind::OUTPUT
        : ToolCall::Result::Kind::ERROR;
    _post([this, req, kind, out] {
        ToolCall::Result result { kind, out.text };
        result.diff = out.diff;
        session_->fill_tool_result(req, std::move(result));
    });
    tool_msgs.push_back({ Message::Type::TOOL, out.text, { }, req.id });
}

} // namespace ursa
