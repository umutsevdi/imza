#include "platform/json_file.h"

#include "platform/file_lock.h"

#include <fstream>
#include <sstream>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#include <cerrno>
#include <chrono>
#include <system_error>
#include <thread>
#endif

namespace imza {

namespace {

    std::optional<std::string> read_text_once(const std::filesystem::path& path)
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

} // namespace

std::optional<std::string> read_text_file(const std::filesystem::path& path)
{
#ifdef _WIN32
    // Filter drivers (antivirus, search indexing) briefly deny opens of
    // files written moments ago; retry while the file still exists so a
    // transient lock is not reported like a missing file.
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (auto text = read_text_once(path)) {
            return text;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return std::nullopt;
#else
    return read_text_once(path);
#endif
}

std::optional<JsonValue> read_json_file(const std::filesystem::path& path)
{
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        return std::nullopt;
    }
    JsonValue root;
    if (glz::read<JSON_READ>(root, *text)) {
        return std::nullopt;
    }
    return root;
}

Status mutate_json_file(const std::filesystem::path& path,
    const std::function<bool(JsonValue&)>& mutate)
{
    auto lock = acquire_file_lock(lock_path_for(path));
    if (!std::holds_alternative<FileLock>(lock)) {
        return Status::CONFIG_ERROR;
    }
    JsonValue root = JsonValue::object_t { };
    if (auto stored = read_json_file(path)) {
        root = std::move(*stored);
    }
    if (!mutate(root)) {
        return Status::OK;
    }
    return write_json_file(path, json_dump(root));
}

Status write_json_file(
    const std::filesystem::path& path, std::string_view serialized)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Status::CONFIG_ERROR;
    }
#ifndef _WIN32
    // The data directory holds credentials and transcripts; keep it private.
    std::filesystem::permissions(path.parent_path(),
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, ec);
    ec.clear();
#endif

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Status::CONFIG_ERROR;
        }
        file << serialized << '\n';
        if (!file) {
            return Status::CONFIG_ERROR;
        }
    }
#ifdef _WIN32
    // Filter drivers (antivirus, search indexing) hold just-written files
    // open for a short scan; the rename then fails with a sharing
    // violation. Retry the bounded window before reporting the error.
    bool moved = false;
    for (int attempt = 0; attempt < 80 && !moved; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            moved = true;
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            ec = std::error_code(
                static_cast<int>(error), std::system_category());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!moved && !ec) {
        ec = std::error_code(EACCES, std::generic_category());
    }
#else
    // Owner-only; the rename carries this mode onto the destination.
    std::filesystem::permissions(tmp,
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (!ec) {
        std::filesystem::rename(tmp, path, ec);
    }
#endif
    return ec ? Status::CONFIG_ERROR : Status::OK;
}

} // namespace imza
