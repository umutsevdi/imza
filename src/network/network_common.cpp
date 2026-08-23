#include "network.h"

namespace ursa {

std::string write_json(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value parse_json(std::string_view text)
{
    Json::Value value;
    Json::CharReaderBuilder reader;
    std::string err;
    const std::string copy(text);
    std::istringstream stream(copy);
    if (!Json::parseFromStream(reader, stream, &value, &err)) {
        return Json::Value::null;
    }
    return value;
}

const char* role_str(Message::Type type)
{
    switch (type) {
    case Message::Type::SYSTEM: return "system";
    case Message::Type::USER: return "user";
    case Message::Type::ASSISTANT: return "assistant";
    }
    return "user";
}

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

StreamEvent make_error_event(Status error)
{
    StreamEvent ev;
    ev.kind  = StreamEvent::Kind::ERROR;
    ev.error = error;
    return ev;
}

} // namespace ursa
