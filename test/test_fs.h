#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "platform/file_lock.h"
#include "test_platform.h"

namespace imza::test {

// Releases a held lock variant from acquire_file_lock by resetting it to
// the error state.
template <typename LockVariant> void release_lock(LockVariant& held)
{
    held = LockVariant { imza::FileLockError { } };
}

// RAII scratch directory: unique per instance under the system temp path,
// created eagerly, removed with everything inside it on destruction —
// including when an assertion fails mid-test.
struct TempDir {
    TempDir()
        : path(std::filesystem::temp_directory_path()
              / ("imza-test-" + std::to_string(process_id()) + "-"
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

// RAII restoration of the process working directory: a test that chdirs
// cannot leak the change into the tests that follow when an assertion
// fails before its manual restore runs.
class CurrentDirectory {
public:
    CurrentDirectory()
        : original(std::filesystem::current_path())
    {
    }

    ~CurrentDirectory()
    {
        std::error_code ec;
        std::filesystem::current_path(original, ec);
    }

    CurrentDirectory(const CurrentDirectory&)            = delete;
    CurrentDirectory& operator=(const CurrentDirectory&) = delete;

    const std::filesystem::path original;
};

// The variables data_dir() reads to locate Imza's state root. Windows and
// macOS resolve through APPDATA/HOME rather than XDG, so isolating only the
// XDG variables would leave those platforms writing to the real profile.
inline const char* data_home_variable()
{
#ifdef _WIN32
    return "APPDATA";
#elif defined(__APPLE__)
    return "HOME";
#else
    return "XDG_DATA_HOME";
#endif
}

inline const char* config_home_variable()
{
#ifdef _WIN32
    return "APPDATA";
#elif defined(__APPLE__)
    return "HOME";
#else
    return "XDG_CONFIG_HOME";
#endif
}

// RAII isolation of Imza's data home: a fresh TempDir is exported through the
// variable data_dir() reads on this platform, and the previous value is
// restored on destruction, so a suite cannot leak it process-wide into the
// ones that follow.
class IsolatedDataHome {
public:
    IsolatedDataHome()
        : _previous(env_value(data_home_variable()))
        , _had_previous(!_previous.empty())
    {
        set_env(data_home_variable(), _root.path.string());
    }

    ~IsolatedDataHome()
    {
        if (_had_previous) {
            set_env(data_home_variable(), _previous);
        } else {
            unset_env(data_home_variable());
        }
    }

    IsolatedDataHome(const IsolatedDataHome&)            = delete;
    IsolatedDataHome& operator=(const IsolatedDataHome&) = delete;

    // The exported root; Imza's files live under imza_dir().
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

// RAII isolation of the config home: a fresh TempDir is exported through the
// variable the environment scan reads on this platform. Imza discovers global
// skills under `<config-home>/agents/skills`.
class IsolatedConfigHome {
public:
    IsolatedConfigHome()
        : _previous(env_value(config_home_variable()))
        , _had_previous(!_previous.empty())
    {
        set_env(config_home_variable(), _root.path.string());
        // The scan also walks `<home>/.agents/skills` and friends; point HOME
        // at the scratch root so a developer's real skills never leak in.
        _home = std::make_unique<ScopedEnv>("HOME", _root.path.string());
    }

    ~IsolatedConfigHome()
    {
        if (_had_previous) {
            set_env(config_home_variable(), _previous);
        } else {
            unset_env(config_home_variable());
        }
    }

    IsolatedConfigHome(const IsolatedConfigHome&)            = delete;
    IsolatedConfigHome& operator=(const IsolatedConfigHome&) = delete;

    std::filesystem::path root() const { return _root.path; }

    // Creates `<root>/agents/skills/<name>/SKILL.md` with a description.
    std::filesystem::path add_skill(
        const std::string& name, const std::string& description) const
    {
        const std::filesystem::path skill
            = _root.path / "agents" / "skills" / name;
        write_file(skill / "SKILL.md",
            "---\ndescription: " + description + "\n---\n\nBody.\n");
        return skill / "SKILL.md";
    }

private:
    TempDir _root;
    std::string _previous;
    bool _had_previous = false;
    std::unique_ptr<ScopedEnv> _home;
};

} // namespace imza::test
