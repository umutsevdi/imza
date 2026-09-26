#include "conversation/input_history.h"

#include "platform/json_file.h"

#include <json/json.h>

#include <utility>

namespace imza {

namespace {

    std::vector<std::string> parse_entries(const Json::Value& root)
    {
        if (!root.isObject() || root.get("version", 0).asInt() != 1
            || !root["entries"].isArray()) {
            return { };
        }
        std::vector<std::string> entries;
        for (const Json::Value& entry : root["entries"]) {
            if (!entry.isString()) {
                return { };
            }
            entries.push_back(entry.asString());
        }
        return entries;
    }

    Json::Value encode_entries(const std::vector<std::string>& entries)
    {
        Json::Value root(Json::objectValue);
        root["version"] = 1;
        Json::Value values(Json::arrayValue);
        for (const std::string& entry : entries) {
            values.append(entry);
        }
        root["entries"] = std::move(values);
        return root;
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
    if (_entries.size() > _limit) {
        _entries.erase(_entries.begin(),
            _entries.end() - static_cast<std::ptrdiff_t>(_limit));
    }
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
    const Status status = mutate_json_file(_path, [&](Json::Value& root) {
        entries = parse_entries(root);
        if (entries.empty() || entries.back() != text) {
            entries.push_back(std::move(text));
        }
        if (entries.size() > _limit) {
            entries.erase(entries.begin(),
                entries.end() - static_cast<std::ptrdiff_t>(_limit));
        }
        root = encode_entries(entries);
        return true;
    });
    if (status == Status::OK) {
        _entries = std::move(entries);
    }
    return status;
}

} // namespace imza
