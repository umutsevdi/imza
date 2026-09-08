#include "platform/json_file.h"

#include <fstream>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

namespace imza {

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
