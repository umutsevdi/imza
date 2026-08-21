#include "network.h"

namespace ursa {

std::string strip_slash(std::string_view base)
{
    std::string out(base);
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

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

} // namespace ursa
