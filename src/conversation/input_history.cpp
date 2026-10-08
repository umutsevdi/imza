#include "conversation/input_history.h"

#include "network/json.h"
#include "platform/json_file.h"

#include <utility>

namespace imza {

namespace {

    std::vector<std::string> parse_entries(const JsonValue& root)
    {
        const JsonValue* version = find_member(root, "version");
        const JsonValue* entries = find_member(root, "entries");
        if (version == nullptr || !version->is_number()
            || version->as<int>() != 1 || entries == nullptr
            || !entries->is_array()) {
            return { };
        }
        std::vector<std::string> parsed;
        for (const JsonValue& entry : entries->get<JsonValue::array_t>()) {
            if (!entry.is_string()) {
                return { };
            }
            parsed.push_back(entry.as<std::string>());
        }
        return parsed;
    }

    JsonValue encode_entries(const std::vector<std::string>& entries)
    {
        JsonValue root    = JsonValue::object_t { };
        auto& object      = root.get<JsonValue::object_t>();
        object["version"] = JsonValue(1.0);
        JsonValue values  = JsonValue::array_t { };
        auto& array       = values.get<JsonValue::array_t>();
        for (const std::string& entry : entries) {
            array.emplace_back(entry);
        }
        object["entries"] = std::move(values);
        return root;
    }

    void trim_to_limit(std::vector<std::string>& entries, std::size_t limit)
    {
        if (entries.size() > limit) {
            entries.erase(entries.begin(),
                entries.end() - static_cast<std::ptrdiff_t>(limit));
        }
    }

} // namespace

InputHistoryStore::InputHistoryStore(
    std::filesystem::path path, std::size_t limit)
    : _path(std::move(path))
    , _limit(limit)
{
    if (auto root = read_json_file(_path)) {
        _entries = parse_entries(*root);
    }
    trim_to_limit(_entries, _limit);
}

std::vector<std::string> InputHistoryStore::entries() const
{
    std::lock_guard lock(_mutex);
    return _entries;
}

Status InputHistoryStore::record(std::string text)
{
    std::lock_guard lock(_mutex);
    std::vector<std::string> entries;
    const Status status = mutate_json_file(_path, [&](JsonValue& root) {
        entries = parse_entries(root);
        if (entries.empty() || entries.back() != text) {
            entries.push_back(std::move(text));
        }
        trim_to_limit(entries, _limit);
        root = encode_entries(entries);
        return true;
    });
    if (status == Status::OK) {
        _entries = std::move(entries);
    }
    return status;
}

} // namespace imza
