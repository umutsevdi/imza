#include "io.h"

#include <fstream>

namespace ursa {

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
        file << Json::writeString(builder, root) << "\n";
        if (!file) {
            return Status::CONFIG_ERROR;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    return ec ? Status::CONFIG_ERROR : Status::OK;
}

} // namespace ursa
