#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/types.h"
#include "network/chat.h"

namespace imza {

class Session;
struct LoadedSession;

// Aggregate of saved usage grouped per (provider, model); built by
// rollup_usage_by_model and rendered by the /usage tables.
struct UsageBreakdownRow {
    std::string provider;
    std::string model;
    std::uint64_t prompt     = 0;
    std::uint64_t completion = 0;
    std::uint64_t total      = 0;
    double cost              = 0.0;
};

struct GlobalUsageSummary {
    std::uint64_t prompt     = 0;
    std::uint64_t completion = 0;
    std::uint64_t total      = 0;
    double total_cost        = 0.0;
    std::vector<UsageBreakdownRow> by_model;
    // Spend per day, ordered by day ascending.
    std::vector<UsageDayRow> by_day;
};

// Accumulates usage entries into per-(provider, model) rows, ordered by
// provider then model. The one rollup shared by persistence, the global
// summary, and the /usage Session tab.
std::vector<UsageBreakdownRow> rollup_usage_by_model(
    const std::vector<UsageEntry>& entries);

// Totals a usage log: sums plus the shared rollup. Cost is carried by the
// entries; nothing is re-computed.
// Aggregates a usage log; entries without a timestamp land on
// `fallback_day` (the session's save date).
GlobalUsageSummary summarize_usage(
    const std::vector<UsageEntry>& entries, std::string fallback_day);

// Title shown for sessions saved without a user-set name.
inline constexpr char UNTITLED_TITLE[] = "Untitled session";

Status save_session(Session& session);
// One-shot (`--ask`/`--exec`) exit save: same serialization as
// save_session, written under unattended_dir() and never added to the
// session index, so it stays invisible to the session picker.
Status save_unattended_session(Session& session);
Status read_session(const std::filesystem::path& path, LoadedSession& loaded);
std::vector<SavedSession> saved_sessions();
// The subset created by one client: an empty `client` selects terminal
// sessions, a non-empty one selects that ACP client's sessions.
std::vector<SavedSession> saved_sessions_of(std::string_view client);
// Sums the given saved sessions' usage summaries into a per-(provider,
// model) breakdown and a per-day spend series (days "YYYY-MM-DD",
// ordered ascending). Entries without a usage summary count as zero.
GlobalUsageSummary summarize_saved_usage(
    const std::vector<SavedSession>& saved);
// The same over every session found in the index.
GlobalUsageSummary global_usage_summary();
bool session_file_locked(const std::filesystem::path& path);

enum class DeleteSessionResult { OK, INVALID_PATH, REMOVE_FAILED };
DeleteSessionResult delete_saved_session(const std::filesystem::path& path);

struct TrimResult {
    std::size_t regular_removed    = 0;
    std::size_t unattended_removed = 0;
    std::size_t skipped_locked     = 0;
    std::size_t skipped_invalid    = 0;
};

// Startup sweep: deletes every regular session and unattended transcript
// whose save time is older than `age` before `now`. The active session's
// file (`keep`) and any file another imza process holds are left alone;
// an unparseable save time keeps the file. A zero `age` trims nothing.
TrimResult trim_saved_sessions(std::chrono::days age,
    std::chrono::system_clock::time_point now,
    const std::filesystem::path& keep = { });

} // namespace imza
