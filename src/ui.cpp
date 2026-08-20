#include "ui.hpp"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "network.hpp"
#include "render.hpp"

namespace ursa {

namespace {

using namespace ftxui;

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

enum class Phase { IDLE, STREAMING };

struct Turn {
    Message::Type role;
    std::string content;
};

std::string error_text(Status st)
{
    return "stream error (" + std::to_string(static_cast<int>(st)) + ")";
}

// ---- pure render helpers (no state) ---------------------------------------

Element user_turn(const Turn& t)
{
    return hbox({
        text("you › ") | color(Color::GreenLight) | bold,
        paragraph(t.content),
    });
}

Element assistant_label()
{
    return text("ursa › ") | color(Color::CyanLight) | bold;
}

Element assistant_body(std::string_view markdown)
{
    return vbox({
        assistant_label(),
        render_markdown_element(markdown),
    });
}

Element turn_element(const Turn& t)
{
    return t.role == Message::Type::USER ? user_turn(t)
                                         : assistant_body(t.content);
}

Element header_element(const Config& cfg)
{
    return hbox({
               text(" ursa ") | bold | color(Color::White),
               text(cfg.model.empty() ? "" : " · " + cfg.model + " ")
                   | color(Color::GrayDark),
               filler(),
           })
        | bgcolor(Color::Palette256(237));
}

// ---- the single REPL component ---------------------------------------------
//
// Owns all UI state and drives the streaming worker. The worker thread only
// ever crosses back to the UI thread through App::Post (documented
// thread-safe); it never touches component state directly, so there is no
// shared mutable state and no manual PostEvent/repaint hack — a repaint is
// requested via animation::RequestAnimationFrame() after every mutation.
class Chat : public ComponentBase {
public:
    Chat(App& screen, const Config& cfg)
        : screen_(screen)
        , cfg_(cfg)
    {
        input_options_.content     = &input_buf_;
        input_options_.placeholder = "ask anything — /exit quits";
        input_options_.multiline   = false;
        input_options_.on_enter    = [this] { submit(); };
        input_                     = ftxui::Input(input_options_);
        Add(input_);
        input_->TakeFocus();
    }

    Element OnRender() override
    {
        Elements items;
        for (const auto& t : turns_) {
            if (!items.empty()) {
                items.push_back(text(""));
            }
            items.push_back(turn_element(t));
        }
        if (phase_ == Phase::STREAMING) {
            if (!items.empty()) {
                items.push_back(text(""));
            }
            items.push_back(assistant_body(streaming_));
        }

        Element content
            = items.empty() ? text("") : vbox(std::move(items)) | flex;
        Element log = std::move(content)
            | vscroll_indicator             //
            | focusPositionRelative(0.0F, scroll_y_)
            | frame;

        Element status = text("");
        if (!error_.empty()) {
            status = hbox({
                text("! ") | bold | color(Color::RedLight),
                text(error_) | color(Color::RedLight),
            });
        } else if (phase_ == Phase::STREAMING) {
            status = hbox({
                spinner(15, static_cast<size_t>(frame_))
                    | color(Color::CyanLight),
                text(" thinking…") | dim,
            });
        }

        Element prompt = hbox({
                       text(" "),
                       input_->Render() | flex,
                       text(" "),
                       status,
                       text(" "),
                   })
            | borderRounded;

        return vbox({
            header_element(cfg_),
            std::move(log) | flex,
            separator(),
            std::move(prompt),
        });
    }

    bool OnEvent(Event event) override
    {
        if (event == Event::CtrlC || event == Event::CtrlD) {
            screen_.Exit();
            return true;
        }
        if (event.is_mouse()) {
            const Mouse& m = event.mouse();
            if (m.button == Mouse::WheelUp) {
                scroll_by(-0.04F);
                return true;
            }
            if (m.button == Mouse::WheelDown) {
                scroll_by(0.04F);
                return true;
            }
            return false;
        }
        if (event == Event::ArrowUp) {
            scroll_by(-0.05F);
            return true;
        }
        if (event == Event::ArrowDown) {
            scroll_by(0.05F);
            return true;
        }
        if (event == Event::PageUp) {
            scroll_by(-0.35F);
            return true;
        }
        if (event == Event::PageDown) {
            scroll_by(0.35F);
            return true;
        }
        // Everything else (typing, Enter) goes to the input.
        return input_->OnEvent(event);
    }

    void OnAnimation(animation::Params&) override
    {
        if (phase_ != Phase::STREAMING) {
            return;
        }
        ++frame_;
        animation::RequestAnimationFrame(); // keep the spinner alive
    }

private:
    void scroll_by(float delta)
    {
        scroll_y_ = std::clamp(scroll_y_ + delta, 0.0F, 1.0F);
    }

    void stick_to_bottom()
    {
        if (scroll_y_ >= 0.999F) {
            scroll_y_ = 1.0F;
        }
    }

    void submit()
    {
        if (phase_ != Phase::IDLE) {
            return;
        }
        const std::string text(trim(input_buf_));
        input_buf_.clear();
        if (text.empty()) {
            return;
        }
        if (text == "/exit" || text == "/quit") {
            screen_.Exit();
            return;
        }

        turns_.push_back({ Message::Type::USER, text });
        error_.clear();
        streaming_.clear();
        phase_    = Phase::STREAMING;
        scroll_y_  = 1.0F;
        animation::RequestAnimationFrame();

        ChatRequest req;
        req.model = cfg_.model;
        req.messages.push_back(
            { Message::Type::SYSTEM, "You are a helpful assistant." });
        for (const auto& t : turns_) {
            req.messages.push_back({ t.role, t.content });
        }

        // The worker runs the blocking HTTP call off the UI thread. It only
        // calls back onto the UI thread through App::Post, so it never reads
        // or writes any member directly.
        worker_.emplace([this, req = std::move(req)]() mutable {
            auto on_event = [this](const StreamEvent& ev) {
                screen_.Post([this, ev] { apply(ev); });
            };
            const Status st = chat_stream(cfg_, req, on_event);
            if (st != Status::OK) {
                screen_.Post([this, st] { finish(error_text(st)); });
            }
        });
    }

    // Runs on the UI thread (via Post). Mutates state, then requests a
    // repaint — a bare Post closure does not invalidate the frame.
    void apply(const StreamEvent& ev)
    {
        switch (ev.kind) {
        case StreamEvent::Kind::CONTENT_DELTA:
            streaming_ += ev.text;
            stick_to_bottom();
            break;
        case StreamEvent::Kind::DONE:
            finish("");
            return;
        case StreamEvent::Kind::ERROR:
            finish(error_text(ev.error));
            return;
        }
        animation::RequestAnimationFrame();
    }

    // Finalize a turn. Guarded so a late error after DONE can't double-apply.
    void finish(std::string error)
    {
        if (phase_ != Phase::STREAMING) {
            return;
        }
        if (!error.empty()) {
            error_ = std::move(error);
        } else {
            turns_.push_back(
                { Message::Type::ASSISTANT, std::move(streaming_) });
            streaming_.clear();
        }
        phase_ = Phase::IDLE;
        stick_to_bottom();
        animation::RequestAnimationFrame();
    }

    App& screen_;
    const Config cfg_;

    std::string input_buf_;
    InputOption input_options_;
    Component input_;

    std::vector<Turn> turns_;
    std::string streaming_;
    std::string error_;
    Phase phase_ = Phase::IDLE;
    float scroll_y_ = 1.0F;
    int frame_ = 0;

    std::optional<std::jthread> worker_;
};

} // namespace

int run_repl(const Config& cfg)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::println("ursa requires an interactive terminal");
        return 1;
    }

    App screen = App::FullscreenAlternateScreen();
    auto chat  = ftxui::Make<Chat>(screen, cfg);
    screen.Loop(chat);
    return 0;
}

} // namespace ursa
