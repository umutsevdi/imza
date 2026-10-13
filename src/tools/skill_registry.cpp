#include "tools/skill_registry.h"

#include "common/util.h"
#include "network/json.h"
#include "network/network.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

namespace imza {

// Glaze-reflected: must have external linkage (Clang/MSVC requirement).
struct StoredSearchEntry {
    std::optional<std::string> id;
    std::optional<std::string> source;
    std::optional<std::string> skillId;
    std::optional<std::string> name;
    std::optional<double> installs;
};

struct StoredSearch {
    std::vector<StoredSearchEntry> skills;
};

struct StoredDownloadEntry {
    std::optional<std::string> path;
    std::optional<std::string> contents;
};

struct StoredDownload {
    std::vector<StoredDownloadEntry> files;
};

namespace {

    constexpr long SEARCH_TIMEOUT_SECS   = 30;
    constexpr long DOWNLOAD_TIMEOUT_SECS = 60;
    // A skill folder is instructions plus supporting assets; keep the
    // install bounded so a hostile payload cannot fill the disk.
    constexpr std::size_t MAX_SKILL_BYTES = 4 * 1024 * 1024;
    constexpr std::size_t MAX_SKILL_FILES = 256;

    const std::string& user_agent()
    {
        static const std::string ua = "User-Agent: imza";
        return ua;
    }

    bool write_file(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(file);
    }

    // Rejects absolute paths, drive letters, and any `..` component.
    bool safe_relative_path(std::string_view raw, std::filesystem::path& out)
    {
        const std::filesystem::path path = path_from_utf8(raw);
        if (path.empty() || path.is_absolute() || path.has_root_name()
            || path.has_root_directory()) {
            return false;
        }
        for (const auto& part : path) {
            if (part == "..") {
                return false;
            }
        }
        out = path.lexically_normal();
        return !out.empty();
    }

} // namespace

std::vector<RemoteSkill> parse_skill_search(std::string_view body)
{
    StoredSearch payload;
    if (json_parse_checked(body, payload)) {
        return { };
    }
    std::vector<RemoteSkill> out;
    out.reserve(payload.skills.size());
    for (auto& entry : payload.skills) {
        RemoteSkill skill;
        skill.id     = entry.id.value_or("");
        skill.source = entry.source.value_or("");
        skill.slug   = entry.skillId.value_or("");
        skill.name   = entry.name.value_or(skill.slug);
        if (skill.id.empty() || skill.slug.empty()) {
            continue;
        }
        if (entry.installs && *entry.installs > 0) {
            skill.installs = static_cast<std::uint64_t>(*entry.installs);
        }
        out.push_back(std::move(skill));
    }
    return out;
}

std::vector<RemoteSkillFile> parse_skill_download(std::string_view body)
{
    StoredDownload payload;
    if (json_parse_checked(body, payload)) {
        return { };
    }
    std::vector<RemoteSkillFile> out;
    out.reserve(payload.files.size());
    for (auto& entry : payload.files) {
        if (!entry.path || !entry.contents) {
            continue;
        }
        out.push_back(RemoteSkillFile {
            std::move(*entry.path), std::move(*entry.contents) });
    }
    return out;
}

Status install_skill_files(const std::filesystem::path& root,
    std::string_view slug, const std::vector<RemoteSkillFile>& files,
    std::string& detail)
{
    detail.clear();
    if (slug.empty() || files.empty()) {
        detail = "the skill has no files";
        return Status::API_ERROR;
    }
    if (files.size() > MAX_SKILL_FILES) {
        detail = "the skill has too many files";
        return Status::API_ERROR;
    }
    std::size_t total = 0;
    bool has_skill_md = false;
    for (const RemoteSkillFile& file : files) {
        total += file.contents.size();
        if (total > MAX_SKILL_BYTES) {
            detail = "the skill is too large";
            return Status::API_ERROR;
        }
        std::filesystem::path relative;
        if (!safe_relative_path(file.path, relative)) {
            detail = "the skill contains an unsafe path";
            return Status::API_ERROR;
        }
        if (relative == "SKILL.md") {
            has_skill_md = true;
        }
    }
    if (!has_skill_md) {
        detail = "the skill has no SKILL.md";
        return Status::API_ERROR;
    }

    const std::filesystem::path target = root / path_from_utf8(slug);
    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    for (const RemoteSkillFile& file : files) {
        std::filesystem::path relative;
        if (!safe_relative_path(file.path, relative)) {
            continue; // already validated above
        }
        const std::filesystem::path destination = target / relative;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            detail = "cannot create the skill directory";
            return Status::CONFIG_ERROR;
        }
        if (!write_file(destination, file.contents)) {
            detail = "cannot write " + relative.string();
            return Status::CONFIG_ERROR;
        }
    }
    return Status::OK;
}

Status remove_skill_files(
    const std::filesystem::path& skill_md_path, std::string& detail)
{
    detail.clear();
    if (skill_md_path.filename() != "SKILL.md") {
        detail = "not a skill path";
        return Status::CONFIG_ERROR;
    }
    const std::filesystem::path directory = skill_md_path.parent_path();
    if (directory.empty() || directory == directory.root_path()) {
        detail = "refusing to remove this directory";
        return Status::CONFIG_ERROR;
    }
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    if (ec) {
        detail = "cannot remove the skill";
        return Status::CONFIG_ERROR;
    }
    return Status::OK;
}

SkillRegistry::SkillRegistry(SkillRegistryHooks hooks)
    : _hooks(std::move(hooks))
{
}

SkillRegistry::~SkillRegistry()
{
    _stopping.store(true);
    for (;;) {
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(_workers_mutex);
            workers.swap(_workers);
        }
        if (workers.empty()) {
            break;
        }
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
}

void SkillRegistry::spawn(std::function<void()> work)
{
    std::lock_guard lock(_workers_mutex);
    _workers.emplace_back(std::move(work));
}

bool SkillRegistry::busy() const { return _running.load(); }

void SkillRegistry::search(std::string query)
{
    query = std::string(trim(query));
    if (query.size() < 2 || _running.exchange(true)) {
        return;
    }
    {
        std::lock_guard lock(_mutex);
        _snapshot.phase = Phase::SEARCHING;
        _snapshot.query = query;
        _snapshot.results.clear();
        _snapshot.status    = "Searching skills.sh…";
        _snapshot.status_ok = true;
        _snapshot.active.clear();
    }
    _changed.publish();
    spawn([this, query = std::move(query)] { run_search(query); });
}

void SkillRegistry::install(const RemoteSkill& skill)
{
    if (skill.id.empty() || _running.exchange(true)) {
        return;
    }
    {
        std::lock_guard lock(_mutex);
        _snapshot.phase     = Phase::INSTALLING;
        _snapshot.active    = skill.id;
        _snapshot.status    = "Installing " + skill.name + "…";
        _snapshot.status_ok = true;
    }
    _changed.publish();
    spawn([this, skill] { run_install(skill); });
}

void SkillRegistry::remove_path(std::filesystem::path skill_md_path)
{
    if (_running.exchange(true)) {
        return;
    }
    {
        std::lock_guard lock(_mutex);
        _snapshot.phase     = Phase::REMOVING;
        _snapshot.active    = skill_md_path.parent_path().filename().string();
        _snapshot.status    = "Removing " + _snapshot.active + "…";
        _snapshot.status_ok = true;
    }
    _changed.publish();
    spawn([this, path = std::move(skill_md_path)] { run_remove(path); });
}

SkillRegistry::Snapshot SkillRegistry::snapshot() const
{
    std::lock_guard lock(_mutex);
    return _snapshot;
}

bool SkillRegistry::installed(std::string_view slug) const
{
    std::error_code ec;
    return std::filesystem::is_regular_file(
        _hooks.install_root / path_from_utf8(slug) / "SKILL.md", ec);
}

Signal<>::Subscription SkillRegistry::subscribe(Signal<>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

void SkillRegistry::run_search(std::string query)
{
    std::string body;
    long code             = 0;
    const std::string url = _hooks.base_url
        + "/api/search?q=" + percent_encode(query) + "&limit=30";
    const Status st
        = http_get(url, { user_agent() }, SEARCH_TIMEOUT_SECS, body, &code);

    std::vector<RemoteSkill> results;
    std::string status;
    bool ok = true;
    if (st != Status::OK) {
        status = "Search failed: network error.";
        ok     = false;
    } else if (!http_ok(code)) {
        status = "Search failed: HTTP " + std::to_string(code) + ".";
        ok     = false;
    } else {
        results = parse_skill_search(body);
        if (results.empty()) {
            status = "No skills found for “" + query + "”.";
        } else {
            status = std::to_string(results.size()) + " result"
                + (results.size() == 1 ? "" : "s");
        }
    }

    {
        std::lock_guard lock(_mutex);
        _snapshot.phase     = Phase::IDLE;
        _snapshot.results   = std::move(results);
        _snapshot.status    = std::move(status);
        _snapshot.status_ok = ok;
        _snapshot.active.clear();
    }
    _running.store(false);
    _changed.publish();
}

void SkillRegistry::run_install(RemoteSkill skill)
{
    std::string body;
    long code             = 0;
    const std::string url = _hooks.base_url + "/api/download/" + skill.id;
    const Status st
        = http_get(url, { user_agent() }, DOWNLOAD_TIMEOUT_SECS, body, &code);

    std::string status;
    bool ok = true;
    if (st != Status::OK) {
        status = "Install failed: network error.";
        ok     = false;
    } else if (!http_ok(code)) {
        status = "Install failed: HTTP " + std::to_string(code) + ".";
        ok     = false;
    } else {
        const std::vector<RemoteSkillFile> files = parse_skill_download(body);
        std::string detail;
        const Status written = install_skill_files(
            _hooks.install_root, skill.slug, files, detail);
        if (written != Status::OK) {
            status = "Install failed: " + detail + ".";
            ok     = false;
        } else {
            status = "Installed " + skill.name + ".";
        }
    }

    if (ok && _hooks.on_mutated) {
        _hooks.on_mutated();
    }
    {
        std::lock_guard lock(_mutex);
        _snapshot.phase     = Phase::IDLE;
        _snapshot.status    = std::move(status);
        _snapshot.status_ok = ok;
        _snapshot.active.clear();
    }
    _running.store(false);
    _changed.publish();
}

void SkillRegistry::run_remove(std::filesystem::path skill_md_path)
{
    std::string detail;
    const Status st = remove_skill_files(skill_md_path, detail);
    std::string status;
    bool ok = st == Status::OK;
    if (ok) {
        status = "Removed " + skill_md_path.parent_path().filename().string()
            + ".";
        if (_hooks.on_mutated) {
            _hooks.on_mutated();
        }
    } else {
        status = "Remove failed: " + detail + ".";
    }
    {
        std::lock_guard lock(_mutex);
        _snapshot.phase     = Phase::IDLE;
        _snapshot.status    = std::move(status);
        _snapshot.status_ok = ok;
        _snapshot.active.clear();
    }
    _running.store(false);
    _changed.publish();
}

} // namespace imza
