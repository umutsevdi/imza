#pragma once

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>

#include "platform/file_lock.h"
#include "providers/catalog.h"
#include "test_platform.h"
#include "workspace/environment.h"

namespace imza::test {

// Releases a held lock variant from acquire_file_lock by resetting it to
// the error state.
template <typename LockVariant> void release_lock(LockVariant& held)
{
    held = LockVariant { imza::FileLockError { } };
}

// RAII scratch directory: unique per instance under the system temp path,
// created eagerly, removed with everything inside it on destruction
// (including when an assertion fails mid-test).
//
// The stored path is canonicalized: macOS exposes the system temp as
// /var/folders/..., a symlink to /private/var/folders/..., while the
// permission evaluator and product code canonicalize paths, so comparisons
// against the raw temp path fail only on macOS.
struct TempDir {
    TempDir()
        : path(std::filesystem::weakly_canonical(
              std::filesystem::temp_directory_path()
              / ("imza-test-" + std::to_string(process_id()) + "-"
                  + std::to_string(++sequence()))))
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

    // The exported root; Imza's files live under imza_dir() which mirrors
    // the per-platform layout of data_dir() (platform/config.cpp): Windows
    // and Linux carry the plain "imza" suffix, macOS nests below Library.
    std::filesystem::path root() const { return _root.path; }
    std::filesystem::path imza_dir() const
    {
#if defined(__APPLE__)
        return _root.path / "Library" / "Application Support" / "imza";
#else
        return _root.path / "imza";
#endif
    }

private:
    TempDir _root;
    std::string _previous;
    bool _had_previous = false;
};

// A data home whose presets file holds one catalog provider row with a
// single model, for tests that need a provider the store can resolve.
class CatalogDataHome : public IsolatedDataHome {
public:
    explicit CatalogDataHome(std::string_view provider_id,
        std::string_view model_id = "catalog-model")
    {
        imza::Catalog catalog;
        catalog.fetched_at = static_cast<std::int64_t>(std::time(nullptr));
        imza::CachedProvider provider;
        provider.npm = "@ai-sdk/openai-compatible";
        provider.models[std::string(model_id)].name = std::string(model_id);
        catalog.providers[std::string(provider_id)] = provider;
        std::ignore = imza::save_catalog(imza_dir() / "presets.json", catalog);
    }
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

class IsolatedConfigHome {
public:
    IsolatedConfigHome()
        : _previous(env_value(config_home_variable()))
        , _had_previous(!_previous.empty())
        , _xdg_config("XDG_CONFIG_HOME", _root.path.string())
        , _home("HOME", _root.path.string())
        , _profile("USERPROFILE", _root.path.string())
    {
        set_env(config_home_variable(), _root.path.string());
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

    // Creates `<config-dir>/agents/skills/<name>/SKILL.md` with a
    // description, through the production resolver so the location follows
    // the platform's config layout (Application Support on macOS).
    std::filesystem::path add_skill(
        const std::string& name, const std::string& description) const
    {
        const std::filesystem::path skill
            = imza::global_skills_directory() / name;
        write_file(skill / "SKILL.md",
            "---\ndescription: " + description + "\n---\n\nBody.\n");
        return skill / "SKILL.md";
    }

private:
    TempDir _root;
    std::string _previous;
    bool _had_previous = false;
    // Reverse member order at destruction: HOME restores first, then
    // XDG_CONFIG_HOME, after the destructor body restores the config-home
    // variable.
    ScopedEnv _xdg_config;
    ScopedEnv _home;
    ScopedEnv _profile;
};

} // namespace imza::test
