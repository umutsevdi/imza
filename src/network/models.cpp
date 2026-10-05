#include "common/util.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/network.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace imza {

namespace {

    constexpr std::array<std::string_view, 5> DENY_LIST
        = { "embed", "whisper", "tts", "dall-e", "moderation" };

    bool denied(std::string_view id)
    {
        const std::string lower = to_lower(id);
        for (std::string_view needle : DENY_LIST) {
            if (lower.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

} // namespace

Status parse_models_response(std::string_view body, std::vector<ModelInfo>& out)
{
    // The "data" array mixes objects with arbitrary entries (strings,
    // nulls) across vendors, so the array stays dynamic and entries that
    // are not objects are skipped rather than failing the parse.
    const JsonValue root = parse_json(body);
    if (!root.is_object()) {
        return Status::JSON_ERROR;
    }
    const JsonValue* data = find_member(root, "data");
    if (data == nullptr || !data->is_array()) {
        return Status::JSON_ERROR;
    }

    out.clear();
    const auto& array = data->get<JsonValue::array_t>();
    out.reserve(array.size());
    for (const JsonValue& entry : array) {
        if (!entry.is_object()) {
            continue;
        }
        const JsonValue* id = find_member(entry, "id");
        if (id == nullptr || !id->is_string()) {
            continue;
        }
        const std::string model_id = id->as<std::string>();
        if (model_id.empty() || denied(model_id)) {
            continue;
        }
        ModelInfo info;
        info.id = model_id;
        if (const JsonValue* name = find_member(entry, "name");
            name != nullptr && name->is_string()) {
            info.name = name->as<std::string>();
        }
        if (const JsonValue* ctx = find_member(entry, "context_length");
            ctx != nullptr && ctx->is_number()) {
            info.context_length = static_cast<std::uint64_t>(ctx->as<double>());
        }
        out.push_back(std::move(info));
    }

    std::sort(out.begin(), out.end(),
        [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
    return Status::OK;
}

Status fetch_models(const Route& route, std::vector<ModelInfo>& out)
{
    if (route.api.empty()) {
        return Status::INVALID_URL;
    }
    std::string url = strip_slash(route.api) + "/models";
    if (route.dialect == ApiStandard::ANTHROPIC) {
        url += "?limit=1000";
    }

    const std::vector<std::string> headers
        = request_headers(route, { "Accept: application/json" });

    std::string body;
    long code       = 0;
    const Status st = http_get(url, headers, 10, body, &code);
    if (st != Status::OK) {
        return st;
    }
    if (!http_ok(code)) {
        return Status::API_ERROR;
    }
    return parse_models_response(body, out);
}

} // namespace imza
