#include "platform/json_file.h"

#include "platform/file_lock.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#endif

namespace imza {

std::optional<std::string> read_text_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad() && !file.eof()) {
        return std::nullopt;
    }
    return buffer.str();
}

std::optional<Json::Value> read_json_file(const std::filesystem::path& path)
{
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        return std::nullopt;
    }
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string error;
    std::istringstream stream { *text };
    if (!Json::parseFromStream(reader, stream, &root, &error)) {
        return std::nullopt;
    }
    return root;
}

Status mutate_json_file(const std::filesystem::path& path,
    const std::function<bool(Json::Value&)>& mutate)
{
    auto lock = acquire_file_lock(lock_path_for(path));
    if (!std::holds_alternative<FileLock>(lock)) {
        return Status::CONFIG_ERROR;
    }
    Json::Value root(Json::objectValue);
    if (auto stored = read_json_file(path)) {
        root = std::move(*stored);
    }
    if (!mutate(root)) {
        return Status::OK;
    }
    return write_json_file(path, root, "");
}

Status write_json_file(const std::filesystem::path& path,
    const Json::Value& root, std::string_view indentation)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Status::CONFIG_ERROR;
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Status::CONFIG_ERROR;
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = std::string(indentation);
        std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
        if (!writer || writer->write(root, &file) != 0) {
            return Status::CONFIG_ERROR;
        }
        file << '\n';
        if (!file) {
            return Status::CONFIG_ERROR;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(
            static_cast<int>(GetLastError()), std::system_category());
    }
#else
    std::filesystem::rename(tmp, path, ec);
#endif
    return ec ? Status::CONFIG_ERROR : Status::OK;
}

} // namespace imza
