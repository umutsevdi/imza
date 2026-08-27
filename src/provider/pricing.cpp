#include "pricing.h"

#include "util.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace ursa {

namespace {

    constexpr double kPerMillionToPerK = 1.0 / 1000.0;

    std::mutex& pricing_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::map<std::string, ModelPricing>& pricing_table()
    {
        static std::map<std::string, ModelPricing> table;
        return table;
    }

    std::optional<ModelPricing> pricing_from_model(const CachedModel& model)
    {
        if (!model.cost_input && !model.cost_output) {
            return std::nullopt;
        }
        ModelPricing pricing;
        pricing.input_per_1k = model.cost_input.value_or(0.0) * kPerMillionToPerK;
        pricing.output_per_1k
            = model.cost_output.value_or(0.0) * kPerMillionToPerK;
        pricing.cache_read_per_1k
            = model.cost_cache_read.value_or(0.0) * kPerMillionToPerK;
        pricing.cache_write_per_1k
            = model.cost_cache_write.value_or(0.0) * kPerMillionToPerK;
        pricing.context_limit = model.context.value_or(0);
        return pricing;
    }

    void insert_pricing(std::map<std::string, ModelPricing>& table,
        const std::string& key, const ModelPricing& pricing)
    {
        table[to_lower(key)] = pricing;
    }

} // namespace

void set_pricing_catalog(const Catalog& catalog)
{
    std::map<std::string, ModelPricing> table;
    for (const auto& [provider_id, provider] : catalog.providers) {
        for (const auto& [model_id, model] : provider.models) {
            const std::optional<ModelPricing> pricing = pricing_from_model(model);
            if (!pricing) {
                continue;
            }
            insert_pricing(table, model_id, *pricing);
            insert_pricing(table, provider_id + "/" + model_id, *pricing);
        }
    }
    std::lock_guard lock(pricing_mutex());
    pricing_table() = std::move(table);
}

ModelPricing get_pricing(std::string_view model_view)
{
    const std::string model = to_lower(model_view);
    if (model.empty()) {
        return { };
    }
    std::lock_guard lock(pricing_mutex());
    const std::map<std::string, ModelPricing>& rows = pricing_table();
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
    const std::int64_t cached_read
        = std::min<std::int64_t>(usage.cached_read, usage.prompt);
    const std::int64_t cached_write = std::min<std::int64_t>(
        usage.cached_write, usage.prompt - cached_read);

    const double read_rate = pricing.cache_read_per_1k > 0.0
        ? pricing.cache_read_per_1k
        : pricing.input_per_1k;
    const double write_rate = pricing.cache_write_per_1k > 0.0
        ? pricing.cache_write_per_1k
        : pricing.input_per_1k;

    const std::int64_t plain_prompt
        = static_cast<std::int64_t>(usage.prompt) - cached_read - cached_write;
    const double plain = (static_cast<double>(plain_prompt) / 1000.0)
        * pricing.input_per_1k;
    const double read = (static_cast<double>(cached_read) / 1000.0) * read_rate;
    const double write
        = (static_cast<double>(cached_write) / 1000.0) * write_rate;
    const double out = (static_cast<double>(usage.completion) / 1000.0)
        * pricing.output_per_1k;
    return plain + read + write + out;
}

} // namespace ursa
