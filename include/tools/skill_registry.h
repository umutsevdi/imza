#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"

namespace imza {

// One skills.sh search hit, as returned by GET /api/search.
struct RemoteSkill {
    std::string id;     // "{source}/{slug}"
    std::string source; // "owner/repo" or a well-known domain
    std::string slug;   // URL-safe skill name
    std::string name;   // human-readable name
    std::uint64_t installs = 0;

    bool operator==(const RemoteSkill&) const = default;
};

// One file of a skill folder, as returned by GET /api/download/{id}.
struct RemoteSkillFile {
    std::string path;
    std::string contents;
};

// Parses the skills.sh search payload; malformed input yields no results.
std::vector<RemoteSkill> parse_skill_search(std::string_view body);

// Parses the skills.sh download payload.
std::vector<RemoteSkillFile> parse_skill_download(std::string_view body);

// Writes `files` under `root/<slug>/`, rejecting absolute paths, `..`
// components, oversized payloads, and folders without a SKILL.md. The
// target directory is replaced, not merged.
Status install_skill_files(const std::filesystem::path& root,
    std::string_view slug, const std::vector<RemoteSkillFile>& files,
    std::string& detail);

// Removes the directory `skill_md_path` lives in; refuses anything that is
// not a SKILL.md inside a named directory.
Status remove_skill_files(
    const std::filesystem::path& skill_md_path, std::string& detail);

// Effects injected at the composition boundary: the API base, where
// installed skills land, and a notification after a successful install or
// removal so the catalogue can rescan.
struct SkillRegistryHooks {
    std::string base_url = "https://skills.sh";
    std::filesystem::path install_root;
    std::function<void()> on_mutated;
};

// Searches and installs skills.sh skills. Long-lived ApplicationComponent:
// network work runs on registry-owned workers and announces through the
// signal; snapshot() returns an immutable copy for the UI thread.
class SkillRegistry final : public ApplicationComponent {
public:
    enum class Phase { IDLE, SEARCHING, INSTALLING, REMOVING };

    struct Snapshot {
        Phase phase = Phase::IDLE;
        std::string query; // the query the current results belong to
        std::vector<RemoteSkill> results;
        std::string status; // progress or failure text
        bool status_ok = true;
        std::string active; // id being installed or removed
    };

    explicit SkillRegistry(SkillRegistryHooks hooks = { });
    ~SkillRegistry();

    SkillRegistry(const SkillRegistry&)            = delete;
    SkillRegistry& operator=(const SkillRegistry&) = delete;

    // Ignored while another operation runs.
    void search(std::string query);
    void install(const RemoteSkill& skill);
    // Removes a discovered skill by its SKILL.md path.
    void remove_path(std::filesystem::path skill_md_path);

    Snapshot snapshot() const;
    // True when the install root holds a directory for `slug`.
    bool installed(std::string_view slug) const;

    [[nodiscard]] Signal<>::Subscription subscribe(Signal<>::Callback callback);

private:
    void spawn(std::function<void()> work);
    void run_search(std::string query);
    void run_install(RemoteSkill skill);
    void run_remove(std::filesystem::path skill_md_path);
    bool busy() const;

    SkillRegistryHooks _hooks;
    mutable std::mutex _mutex;
    Snapshot _snapshot;
    std::atomic_bool _running { false };
    std::atomic_bool _stopping { false };
    std::mutex _workers_mutex;
    std::vector<std::thread> _workers;
    Signal<> _changed;
};

} // namespace imza
