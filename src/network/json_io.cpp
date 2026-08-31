#include "network/json_io.h"

namespace ursa {

std::string write_json(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value parse_json(std::string_view text)
{
    static thread_local Json::CharReaderBuilder builder;
    static thread_local std::unique_ptr<Json::CharReader> reader(
        builder.newCharReader());
    Json::Value value;
    std::string err;
    if (text.empty()
        || !reader->parse(
            text.data(), text.data() + text.size(), &value, &err)) {
        return Json::Value::null;
    }
    return value;
}

} // namespace ursa
