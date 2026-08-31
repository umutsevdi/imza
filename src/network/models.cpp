#include "network.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "util.h"

namespace ursa {

namespace {

    constexpr std::array<std::string_view, 5> kDenyList
        = { "embed", "whisper", "tts", "dall-e", "moderation" };

    bool denied(std::string_view id)
    {
        const std::string lower = to_lower(id);
        for (std::string_view needle : kDenyList) {
            if (lower.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

} // namespace

Status parse_models_response(std::string_view body, std::vector<ModelInfo>& out)
{
    const Json::Value root = parse_json(body);
    if (root.isNull() || !root.isObject()) {
        return Status::JSON_ERROR;
    }
    const Json::Value& data = root["data"];
    if (!data.isArray()) {
        return Status::JSON_ERROR;
    }

    out.clear();
    out.reserve(static_cast<std::size_t>(data.size()));
    for (const Json::Value& entry : data) {
        if (!entry.isObject() || !entry["id"].isString()) {
            continue;
        }
        const std::string id = entry["id"].asString();
        if (id.empty() || denied(id)) {
            continue;
        }
        ModelInfo info;
        info.id = id;
        if (entry["name"].isString()) {
            info.name = entry["name"].asString();
        }
        const Json::Value& ctx = entry["context_length"];
        if (ctx.isUInt64()) {
            info.context_length = ctx.asUInt64();
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

    std::vector<std::string> headers = { "Accept: application/json" };
    for (auto& h : auth_headers(route.auth, route.api_key)) {
        headers.push_back(std::move(h));
    }

    std::string body;
    long code       = 0;
    const Status st = http_get(url, headers, 10, body, &code);
    if (st != Status::OK) {
        return st;
    }
    if (code < 200 || code >= 300) {
        return Status::API_ERROR;
    }
    return parse_models_response(body, out);
}

} // namespace ursa
