#include "tools/file_ops.h"

#include "platform/json_file.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace imza {

bool load_text(const std::string& path, std::string& out, std::string& err)
{
    std::error_code ec;
    const std::filesystem::path file(path);
    if (!std::filesystem::exists(file, ec)) {
        err = "no such file: " + path;
        return false;
    }
    if (!std::filesystem::is_regular_file(file, ec)) {
        err = "not a file: " + path;
        return false;
    }
    std::optional<std::string> content = read_text_file(file);
    if (!content) {
        err = "cannot open: " + path;
        return false;
    }
    if (content->find('\0') != std::string::npos) {
        err = "binary file: " + path;
        return false;
    }
    out = std::move(*content);
    return true;
}

bool save_text(
    const std::string& path, const std::string& content, std::string& err)
{
    // Reached only after permission authorization; a whole-file write may
    // target a not-yet-existing nested path.
    const std::filesystem::path parent
        = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err = "cannot create directory: " + path;
            return false;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write: " + path;
        return false;
    }
    out << content;
    out.close();
    if (!out) {
        err = "cannot write: " + path;
        return false;
    }
    return true;
}

bool make_directories(const std::string& path, std::string& err)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    if (ec) {
        err = "cannot create directory: " + path;
        return false;
    }
    return true;
}

bool remove_path(const std::string& path, std::string& err)
{
    std::error_code ec;
    const std::filesystem::path target(path);
    if (std::filesystem::is_directory(target, ec) && !ec) {
        // rmdir semantics: only an empty directory can be removed.
        if (!std::filesystem::is_empty(target, ec)) {
            err = ec ? "cannot inspect directory: " + path
                     : "directory is not empty: " + path;
            return false;
        }
        std::filesystem::remove(target, ec);
    } else {
        std::filesystem::remove(target, ec);
    }
    if (ec) {
        err = "cannot remove: " + path;
        return false;
    }
    return true;
}

} // namespace imza
