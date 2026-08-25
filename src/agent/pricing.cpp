#include "pricing.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace ursa {

namespace {

std::string to_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

using Row = ModelPricing;

const std::unordered_map<std::string, Row>& table()
{
    static const std::unordered_map<std::string, Row> rows = {
        { "gpt-4o-mini",        { 0.00015, 0.00060, 128000 } },
        { "gpt-4o",             { 0.00250, 0.01000, 128000 } },
        { "gpt-4-turbo",        { 0.01000, 0.03000, 128000 } },
        { "gpt-4",              { 0.03000, 0.06000, 8192 } },
        { "o1",                 { 0.01500, 0.06000, 200000 } },
        { "o3-mini",            { 0.00110, 0.00440, 200000 } },
        { "claude-3-5-sonnet",  { 0.00300, 0.01500, 200000 } },
        { "claude-3-5-haiku",   { 0.00080, 0.00400, 200000 } },
        { "claude-3-opus",      { 0.01500, 0.07500, 200000 } },
        { "claude-3-7-sonnet",  { 0.00300, 0.01500, 200000 } },
        { "claude-sonnet-4",    { 0.00300, 0.01500, 200000 } },
        { "claude-opus-4",      { 0.01500, 0.07500, 200000 } },
        { "glm-4.7-flash",      { 0.00000, 0.00000, 128000 } },
        { "glm-4.5-flash",      { 0.00000, 0.00000, 128000 } },
        { "glm-4.7",            { 0.00050, 0.00150, 128000 } },
        { "glm-4.6",            { 0.00050, 0.00150, 128000 } },
        { "glm-5",              { 0.00060, 0.00150, 128000 } },
        { "kimi-k2",            { 0.00060, 0.00240, 131072 } },
        { "kimi-k1",            { 0.00060, 0.00240, 131072 } },
        { "deepseek-chat",      { 0.00027, 0.00110, 128000 } },
        { "deepseek-reasoner",  { 0.00055, 0.00219, 128000 } },
    };
    return rows;
}

} // namespace

ModelPricing get_pricing(const Config& cfg)
{
    if (cfg.pricing) {
        return *cfg.pricing;
    }
    const std::string model = to_lower(cfg.model);
    if (model.empty()) {
        return { };
    }
    const auto& rows = table();
    auto exact = rows.find(model);
    if (exact != rows.end()) {
        return exact->second;
    }
    for (const auto& [key, row] : rows) {
        if (model.find(key) != std::string::npos) {
            return row;
        }
    }
    return { };
}

double compute_cost(const Usage& usage, const ModelPricing& pricing)
{
    const double in  = (static_cast<double>(usage.prompt) / 1000.0) * pricing.input_per_1k;
    const double out = (static_cast<double>(usage.completion) / 1000.0) * pricing.output_per_1k;
    return in + out;
}

} // namespace ursa
