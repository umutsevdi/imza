#include "network/network.h"
#include "network/sse_parse.h"

#include <utility>

namespace imza {

StreamEvent make_delta_event(std::string text)
{
    StreamEvent ev;
    ev.kind = StreamEvent::Kind::CONTENT_DELTA;
    ev.text = std::move(text);
    return ev;
}

StreamEvent make_tool_call_start_event(ToolCallRequest request)
{
    StreamEvent ev;
    ev.kind      = StreamEvent::Kind::TOOL_CALL_START;
    ev.tool_call = std::move(request);
    return ev;
}
StreamEvent make_tool_call_event(ToolCallRequest request)
{
    StreamEvent ev;
    ev.kind      = StreamEvent::Kind::TOOL_CALL;
    ev.tool_call = std::move(request);
    return ev;
}

StreamEvent make_done_event()
{
    StreamEvent ev;
    ev.kind = StreamEvent::Kind::DONE;
    return ev;
}

StreamEvent make_error_event(Status error, std::string message)
{
    StreamEvent ev;
    ev.kind  = StreamEvent::Kind::ERROR;
    ev.error = error;
    ev.text  = std::move(message);
    return ev;
}

StreamEvent make_usage_event(Usage usage)
{
    StreamEvent ev;
    ev.kind  = StreamEvent::Kind::USAGE;
    ev.usage = usage;
    return ev;
}

StreamEvent make_connected_event()
{
    StreamEvent ev;
    ev.kind = StreamEvent::Kind::CONNECTED;
    return ev;
}

StreamEvent make_reasoning_event(std::string text, std::string signature)
{
    StreamEvent ev;
    ev.kind               = StreamEvent::Kind::REASONING;
    ev.text               = std::move(text);
    ev.thinking_signature = std::move(signature);
    return ev;
}

ToolCallRequest finish_accum(const ToolAccum& acc)
{
    ToolCallRequest req;
    req.name = acc.name;
    req.args = acc.args.empty() ? "{}" : acc.args;
    req.id   = acc.id;
    return req;
}

std::vector<std::string> stream_headers()
{
    return {
        "Content-Type: application/json",
        "Accept: text/event-stream",
    };
}

void flush_tool_accums(ParseState& state, std::vector<StreamEvent>& outs)
{
    for (auto& [index, acc] : state.tool_accums) {
        if (acc.name.empty()) {
            continue;
        }
        outs.push_back(make_tool_call_event(finish_accum(acc)));
    }
    state.tool_accums.clear();
}

void finish_stream(ParseState& state, std::vector<StreamEvent>& outs)
{
    state.terminal = true;
    flush_tool_accums(state, outs);
    outs.push_back(make_done_event());
}

void emit_ready_tool_start(ToolAccum& acc, std::vector<StreamEvent>& outs)
{
    if (acc.started || acc.name.empty()) {
        return;
    }
    acc.started = true;
    outs.push_back(make_tool_call_start_event(finish_accum(acc)));
}

void emit_usage_once(
    ParseState& state, const Usage& usage, std::vector<StreamEvent>& outs)
{
    if (state.usage_emitted
        || (usage.prompt == 0 && usage.completion == 0 && usage.total == 0)) {
        return;
    }
    state.usage_emitted = true;
    outs.push_back(make_usage_event(usage));
}

} // namespace imza
