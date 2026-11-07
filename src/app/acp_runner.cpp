#include "acp/adapter.h"

#include <cstdio>
#include <string>

namespace imza {

namespace {

    // Enforced at the boundary so upstream bugs cannot corrupt framing.
    void write_frame(std::string_view line)
    {
        std::string out(line);
        for (char& c : out) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        out += '\n';
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
    }

} // namespace

int run_acp(std::shared_ptr<ApplicationState> state)
{
    acp::AcpInterface interface(*state, state->providers->config());
    interface.set_writer(write_frame);

    std::string line;
    while (state->alive.load()) {
        // The queue's consumer when the TUI loop is absent.
        state->main_thread->drain();
        const int c = std::getchar();
        if (c == EOF) {
            break;
        }
        if (c != '\n') {
            line += static_cast<char>(c);
            continue;
        }
        if (line.empty()) {
            continue;
        }
        acp::Message message;
        if (!parse_rpc_frame(line, message)) {
            line.clear();
            continue;
        }
        line.clear();
        if (message.method.empty()) {
            interface.handle_response(message);
            continue;
        }
        if (message.id.is_number() || message.id.is_string()) {
            interface.handle_request(
                message.id, message.method, message.params);
        } else {
            interface.handle_notification(message.method, message.params);
        }
    }
    interface.stop();
    return 0;
}

} // namespace imza
