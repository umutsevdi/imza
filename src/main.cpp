#include <print>

#include "network.hpp"
#include "types.hpp"

namespace ursa {

void on_event(const StreamEvent& ev)
{
    switch (ev.kind) {
    case StreamEvent::Kind::CONTENT_DELTA: std::print("{}", ev.text); break;
    case StreamEvent::Kind::DONE: std::println(""); break;
    case StreamEvent::Kind::ERROR: std::println("[stream error]"); break;
    }
}

} // namespace ursa

int main()
{
    const auto path = ursa::config_path();
    ursa::Config cfg;
    const auto status = ursa::load_config(path, cfg);
    if (status != ursa::Status::OK) {
        std::println(
            "config error ({}) at {}", static_cast<int>(status), path.string());
        return 1;
    }

    ursa::ChatRequest req;
    req.model = cfg.model;
    req.messages.push_back(
        { ursa::Message::Type::SYSTEM, "You are a helpful assistant." });
    req.messages.push_back(
        { ursa::Message::Type::USER, "Hello, in one sentence, what are you?" });

    const auto result = ursa::chat_stream(cfg, req, ursa::on_event);
    if (result != ursa::Status::OK) {
        std::println("stream error ({})", static_cast<int>(result));
        return 1;
    }
    return 0;
}
