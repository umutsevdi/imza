#include "ui.hpp"

#include <thread>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace ursa {

namespace {

    std::string_view trim(std::string_view s)
    {
        size_t b = 0;
        size_t e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
            ++b;
        }
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
            --e;
        }
        return s.substr(b, e - b);
    }

} // namespace

std::string error_text(Status st)
{
    return "stream error (" + std::to_string(static_cast<int>(st)) + ")";
}

Controller::Controller(const Config& cfg, PostFn post)
    : cfg_(cfg)
    , post_(std::move(post))
{
    state_.todo = TodoList { {
        { "Implement settings modal", false },
        { "Wire tool calls", false },
        { "Write UI tests", true },
    } };
    state_.changed_files = {
        { "include/ui.hpp", "M" },
        { "src/ui/app.cpp", "M" },
        { "src/ui/changed_files.cpp", "A" },
    };
    state_.question = Question {
        "Which model should I use?",
        { "gpt-4o", "claude-3.5-sonnet", "local/llama3" },
    };
}

void Controller::submit(std::string text)
{
    if (state_.phase != UiState::Phase::IDLE) {
        return;
    }
    const std::string_view t = trim(text);
    if (t.empty()) {
        return;
    }
    if (t == "/exit" || t == "/quit") {
        return;
    }

    state_.items.push_back(UserTurn { std::string(t) });
    state_.error.clear();
    state_.phase = UiState::Phase::STREAMING;

    ChatRequest req;
    req.model = cfg_.model;
    req.messages.push_back({ Message::Type::SYSTEM, "You are a helpful assistant." });
    for (const auto& item : state_.items) {
        if (const auto* u = std::get_if<UserTurn>(&item)) {
            req.messages.push_back({ Message::Type::USER, u->text });
        } else if (const auto* a = std::get_if<AssistantTurn>(&item)) {
            req.messages.push_back({ Message::Type::ASSISTANT, a->markdown });
        }
    }

    state_.items.push_back(AssistantTurn {});

    worker_.emplace([this, req = std::move(req)]() mutable {
        auto on_event = [this](const StreamEvent& ev) {
            post_([this, ev] { apply(ev); });
        };
        const auto provider = get_provider(cfg_);
        const Status st     = stream(provider, cfg_, req, on_event);
        if (st != Status::OK) {
            post_([this, st] { finish(error_text(st)); });
        }
    });
}

void Controller::apply(const StreamEvent& ev)
{
    switch (ev.kind) {
    case StreamEvent::Kind::CONTENT_DELTA:
        if (!state_.items.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&state_.items.back())) {
                a->markdown += ev.text;
            }
        }
        break;
    case StreamEvent::Kind::DONE:
        finish("");
        return;
    case StreamEvent::Kind::ERROR:
        finish(error_text(ev.error));
        return;
    }
}

void Controller::finish(std::string error)
{
    if (state_.phase != UiState::Phase::STREAMING) {
        return;
    }
    if (!error.empty()) {
        state_.error = std::move(error);
    }
    state_.phase = UiState::Phase::IDLE;
}

} // namespace ursa
