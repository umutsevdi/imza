#include "conversation/input_history.h"

#include "platform/file_lock.h"
#include "platform/json_file.h"

#include <json/json.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>
#include <variant>

namespace imza {

namespace {

    std::vector<std::string> read_entries(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return { };
        }
        std::stringstream text;
        text << file.rdbuf();
        Json::CharReaderBuilder reader;
        Json::Value root;
        std::string error;
        if (!Json::parseFromStream(reader, text, &root, &error)
            || !root.isObject() || root.get("version", 0).asInt() != 1
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

    Status write_entries(const std::filesystem::path& path,
        const std::vector<std::string>& entries)
    {
        Json::Value root(Json::objectValue);
        root["version"] = 1;
        Json::Value values(Json::arrayValue);
        for (const std::string& entry : entries) {
            values.append(entry);
        }
        root["entries"] = std::move(values);
        return write_json_file(path, root, "");
    }

} // namespace

InputHistoryStore::InputHistoryStore(
    std::filesystem::path path, std::size_t limit)
    : _path(std::move(path))
    , _limit(limit)
    , _entries(read_entries(_path))
{
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
    auto file_lock = acquire_file_lock(lock_path_for(_path));
    if (!std::holds_alternative<FileLock>(file_lock)) {
        return Status::CONFIG_ERROR;
    }
    std::vector<std::string> entries = read_entries(_path);
    if (entries.empty() || entries.back() != text) {
        entries.push_back(std::move(text));
    }
    if (entries.size() > _limit) {
        entries.erase(entries.begin(),
            entries.end() - static_cast<std::ptrdiff_t>(_limit));
    }
    const Status status = write_entries(_path, entries);
    if (status == Status::OK) {
        _entries = std::move(entries);
    }
    return status;
}

} // namespace imza
