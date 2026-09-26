#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <unistd.h>

namespace imza::test {

// RAII scratch directory: unique per instance under the system temp path,
// created eagerly, removed with everything inside it on destruction —
// including when an assertion fails mid-test.
struct TempDir {
    TempDir()
        : path(std::filesystem::temp_directory_path()
              / ("imza-test-" + std::to_string(::getpid()) + "-"
                  + std::to_string(++sequence())))
    {
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path file(const std::string& name) const
    {
        return path / name;
    }

    const std::filesystem::path path;

private:
    static int& sequence()
    {
        static int value = 0;
        return value;
    }
};

// RAII isolation of Imza's data home: a fresh TempDir is exported as
// XDG_DATA_HOME and the previous value is restored on destruction, so a
// suite cannot leak it process-wide into the ones that follow.
class IsolatedDataHome {
public:
    IsolatedDataHome()
    {
        if (const char* value = std::getenv("XDG_DATA_HOME")) {
            _previous     = value;
            _had_previous = true;
        }
        setenv("XDG_DATA_HOME", _root.path.string().c_str(), 1);
    }

    ~IsolatedDataHome()
    {
        if (_had_previous) {
            setenv("XDG_DATA_HOME", _previous.c_str(), 1);
        } else {
            unsetenv("XDG_DATA_HOME");
        }
    }

    IsolatedDataHome(const IsolatedDataHome&)            = delete;
    IsolatedDataHome& operator=(const IsolatedDataHome&) = delete;

    // The exported XDG_DATA_HOME root; Imza's files live under imza_dir().
    std::filesystem::path root() const { return _root.path; }
    std::filesystem::path imza_dir() const { return _root.path / "imza"; }

private:
    TempDir _root;
    std::string _previous;
    bool _had_previous = false;
};

inline void write_file(
    const std::filesystem::path& path, std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline std::string read_all(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>() };
}

} // namespace imza::test
