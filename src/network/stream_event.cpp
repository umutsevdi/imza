#include "network/network.h"

#include <utility>

namespace ursa {

StreamEvent make_delta_event(std::string text)
{
    StreamEvent ev;
    ev.kind = StreamEvent::Kind::CONTENT_DELTA;
    ev.text = std::move(text);
    return ev;
}

StreamEvent make_tool_call_event(ToolCallRequest request)
{
    StreamEvent ev;
    ev.kind      = StreamEvent::Kind::TOOL_CALL;
    ev.tool_call = std::move(request);
    return ev;
}

StreamEvent make_question_event(QuestionForm form)
{
    StreamEvent ev;
    ev.kind     = StreamEvent::Kind::QUESTION;
    ev.question = std::move(form);
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

} // namespace ursa
