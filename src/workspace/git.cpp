#include "workspace/git.h"

#include "common/util.h"
#include "platform/command_runner.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <optional>
#include <span>
#include <utility>

namespace imza {

namespace {

    constexpr auto GIT_TIMEOUT = std::chrono::seconds { 15 };

    // Stderr is merged: these are user-initiated actions, and git's
    // diagnostic is what the failure message shows.
    GitActionResult git_action(const std::filesystem::path& root,
        const std::string& arguments,
        std::chrono::seconds timeout = GIT_TIMEOUT)
    {
        const CommandResult result
            = run_command(git_command(root, arguments), timeout);
        if (!command_ok(result)) {
            return { false, result.output };
        }
        return { true, result.output };
    }

    std::string quoted_paths(std::span<const std::string> paths)
    {
        std::string out;
        for (const std::string& path : paths) {
            out += " " + shell_quote(path);
        }
        return out;
    }

    ChangedFile::Area changed_file_area(std::string_view status)
    {
        if (status.size() < 2) {
            return ChangedFile::Area::UNSTAGED;
        }
        // '!' is porcelain's ignored marker: not a change on either side.
        const auto in_index = [](char code) {
            return code != ' ' && code != '?' && code != '!';
        };
        const bool staged   = in_index(status[0]);
        const bool unstaged = in_index(status[1]);
        if (staged && unstaged) {
            return ChangedFile::Area::BOTH;
        }
        return staged ? ChangedFile::Area::STAGED : ChangedFile::Area::UNSTAGED;
    }

    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;

    void hash_bytes(std::uint64_t& signature, std::string_view value)
    {
        for (const unsigned char byte : value) {
            signature ^= byte;
            signature *= FNV_PRIME;
        }
    }

    bool has_status(std::string_view status, char value)
    {
        return status.find(value) != std::string_view::npos;
    }

    ChangedFile::Kind changed_file_kind(std::string_view status)
    {
        if (has_status(status, 'U') || status == "AA" || status == "DD") {
            return ChangedFile::Kind::CONFLICTED;
        }
        if (has_status(status, 'D')) {
            return ChangedFile::Kind::DELETED;
        }
        if (has_status(status, 'R')) {
            return ChangedFile::Kind::RENAMED;
        }
        if (has_status(status, 'C')) {
            return ChangedFile::Kind::COPIED;
        }
        if (status == "??") {
            return ChangedFile::Kind::UNTRACKED;
        }
        if (has_status(status, 'A')) {
            return ChangedFile::Kind::ADDED;
        }
        if (has_status(status, 'M') || has_status(status, 'T')) {
            return ChangedFile::Kind::MODIFIED;
        }
        return ChangedFile::Kind::UNKNOWN;
    }

    std::string strip_prefix(std::string path)
    {
        if (path.starts_with("a/") || path.starts_with("b/")) {
            path.erase(0, 2);
        }
        return path;
    }

    std::string header_path(std::string_view line)
    {
        constexpr std::string_view prefix = "diff --git ";
        if (!line.starts_with(prefix)) {
            return { };
        }
        const std::string_view body = line.substr(prefix.size());
        std::size_t split           = body.rfind(" b/");
        if (split == std::string_view::npos) {
            split = body.rfind("\"b/");
        }
        if (split == std::string_view::npos) {
            return { };
        }
        std::string path(body.substr(split + 1));
        if (!path.empty() && path.front() == '"') {
            path.erase(0, 1);
        }
        if (!path.empty() && path.back() == '"') {
            path.pop_back();
        }
        return strip_prefix(std::move(path));
    }

    bool parse_range(
        std::string_view value, std::size_t& start, std::size_t& count)
    {
        const auto start_result
            = std::from_chars(value.data(), value.data() + value.size(), start);
        if (start_result.ec != std::errc { }) {
            return false;
        }
        count = 1;
        if (start_result.ptr == value.data() + value.size()
            || *start_result.ptr != ',') {
            return true;
        }
        const char* count_begin = start_result.ptr + 1;
        const auto count_result
            = std::from_chars(count_begin, value.data() + value.size(), count);
        return count_result.ec == std::errc { };
    }

    bool parse_hunk_header(std::string_view line, ReviewHunk& hunk)
    {
        const std::size_t minus = line.find('-');
        const std::size_t plus  = line.find('+', minus);
        if (minus == std::string_view::npos || plus == std::string_view::npos) {
            return false;
        }
        const std::string_view old_part = line.substr(minus + 1);
        const std::string_view new_part = line.substr(plus + 1);
        return parse_range(old_part, hunk.old_start, hunk.old_count)
            && parse_range(new_part, hunk.new_start, hunk.new_count);
    }

    std::optional<std::filesystem::path> resolve_git_directory(
        const std::filesystem::path& root)
    {
        std::error_code ec;
        std::filesystem::path git_directory = root / ".git";
        if (std::filesystem::is_directory(git_directory, ec) && !ec) {
            return git_directory;
        }
        if (!std::filesystem::is_regular_file(git_directory, ec) || ec) {
            return std::nullopt;
        }
        std::ifstream pointer(git_directory);
        std::string line;
        if (!std::getline(pointer, line)) {
            return std::nullopt;
        }
        std::string_view target(trim(line));
        constexpr std::string_view prefix = "gitdir:";
        if (!target.starts_with(prefix)) {
            return std::nullopt;
        }
        target = trim(target.substr(prefix.size()));
        if (target.empty()) {
            return std::nullopt;
        }
        std::filesystem::path resolved(target);
        if (resolved.is_relative()) {
            resolved = root / resolved;
        }
        return resolved;
    }

} // namespace

std::vector<ChangedFile> parse_git_status(std::string_view status)
{
    std::vector<ChangedFile> files;
    for (std::string line : split_lines(status)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 4 || line[2] != ' ') {
            continue;
        }
        const std::string_view code = std::string_view(line).substr(0, 2);
        files.push_back(ChangedFile {
            line.substr(3), changed_file_kind(code), changed_file_area(code) });
    }
    return files;
}

std::string normalize_git_branch(std::string_view branch)
{
    return std::string(trim(branch));
}

std::optional<std::uint64_t> git_metadata_stamp(
    const std::filesystem::path& root)
{
    std::error_code ec;
    const std::optional<std::filesystem::path> resolved
        = resolve_git_directory(root);
    if (!resolved) {
        return std::nullopt;
    }
    const std::filesystem::path& git_directory = *resolved;

    std::uint64_t stamp = FNV_OFFSET;
    bool found          = false;
    for (const char* name : { "index", "HEAD" }) {
        const auto path = git_directory / name;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto written = std::filesystem::last_write_time(path, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        found = true;
        const auto modified
            = static_cast<long long>(written.time_since_epoch().count());
        hash_bytes(stamp, std::to_string(modified));
        hash_bytes(stamp, std::to_string(size));
    }
    if (!found) {
        return std::nullopt;
    }
    return stamp;
}

bool git_index_locked(
    const std::filesystem::path& root, std::chrono::seconds max_age)
{
    const std::optional<std::filesystem::path> git_directory
        = resolve_git_directory(root);
    if (!git_directory) {
        return false;
    }
    std::error_code ec;
    const auto lock    = *git_directory / "index.lock";
    const auto written = std::filesystem::last_write_time(lock, ec);
    if (ec) {
        return false;
    }
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    return age < max_age;
}

ChangeSummary summarize_git_diff(std::string_view diff)
{
    ChangeSummary summary;
    summary.signature = FNV_OFFSET;
    hash_bytes(summary.signature, diff);
    for (const std::string& line : split_lines(diff)) {
        if (line.empty()) {
            break;
        }
        const std::size_t first_tab = line.find('\t');
        if (first_tab == std::string::npos) {
            break;
        }
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        if (second_tab == std::string::npos) {
            break;
        }
        std::size_t additions   = 0;
        std::size_t deletions   = 0;
        const std::string added = line.substr(0, first_tab);
        const std::string deleted
            = line.substr(first_tab + 1, second_tab - first_tab - 1);
        const auto added_result = std::from_chars(
            added.data(), added.data() + added.size(), additions);
        const auto deleted_result = std::from_chars(
            deleted.data(), deleted.data() + deleted.size(), deletions);
        if (added_result.ec == std::errc { }
            && deleted_result.ec == std::errc { }) {
            summary.additions += additions;
            summary.deletions += deletions;
        }
    }
    return summary;
}

ReviewLoadResult parse_git_diff(std::string_view patch)
{
    RepositoryReview review;
    ReviewFile* file     = nullptr;
    ReviewHunk* hunk     = nullptr;
    std::size_t old_line = 0;
    std::size_t new_line = 0;

    for (std::string line : split_lines(patch)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.starts_with("diff --git ")) {
            review.files.push_back(ReviewFile { });
            file           = &review.files.back();
            file->new_path = header_path(line);
            file->old_path = file->new_path;
            hunk           = nullptr;
            continue;
        }
        if (file == nullptr) {
            if (!line.empty()) {
                return "Invalid git patch: content before file header.";
            }
            continue;
        }
        if (line.starts_with("new file mode ")) {
            file->kind = ReviewFile::Kind::ADDED;
        } else if (line.starts_with("deleted file mode ")) {
            file->kind = ReviewFile::Kind::DELETED;
        } else if (line.starts_with("rename from ")) {
            file->kind     = ReviewFile::Kind::RENAMED;
            file->old_path = line.substr(12);
        } else if (line.starts_with("rename to ")) {
            file->new_path = line.substr(10);
        } else if (line.starts_with("copy from ")) {
            file->kind     = ReviewFile::Kind::COPIED;
            file->old_path = line.substr(10);
        } else if (line.starts_with("copy to ")) {
            file->new_path = line.substr(8);
        } else if (line.starts_with("Binary files ")
            || line.starts_with("GIT binary patch")) {
            file->kind = ReviewFile::Kind::BINARY;
        } else if (line.starts_with("--- ")) {
            const std::string path(line.substr(4));
            if (path != "/dev/null") {
                file->old_path = strip_prefix(path);
            }
        } else if (line.starts_with("+++ ")) {
            const std::string path(line.substr(4));
            if (path != "/dev/null") {
                file->new_path = strip_prefix(path);
            }
        } else if (line.starts_with("@@ ")) {
            file->hunks.push_back(ReviewHunk { line, { } });
            hunk = &file->hunks.back();
            if (!parse_hunk_header(line, *hunk)) {
                return "Invalid git patch: malformed hunk header.";
            }
            old_line = hunk->old_start;
            new_line = hunk->new_start;
        } else if (hunk != nullptr && !line.empty()) {
            if (line.front() == '+') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::ADDITION,
                    std::nullopt, new_line++, line.substr(1) });
                ++file->additions;
            } else if (line.front() == '-') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::DELETION,
                    old_line++, std::nullopt, line.substr(1) });
                ++file->deletions;
            } else if (line.front() == ' ') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::CONTEXT,
                    old_line++, new_line++, line.substr(1) });
            } else if (line.front() == '\\') {
                hunk->lines.push_back(ReviewLine {
                    ReviewLine::Kind::META, std::nullopt, std::nullopt, line });
            }
        }
    }
    return review;
}

UntrackedFileContent read_untracked_file(
    const std::filesystem::path& path, bool keep_lines)
{
    UntrackedFileContent content;
    std::uint64_t signature = FNV_OFFSET;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return content;
    }
    content.readable = true;
    std::array<char, 8192> buffer;
    char last        = '\0';
    bool has_content = false;
    std::string pending;
    while (input) {
        input.read(buffer.data(), buffer.size());
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            continue;
        }
        const std::string_view chunk(
            buffer.data(), static_cast<std::size_t>(count));
        hash_bytes(signature, chunk);
        has_content = true;
        last        = chunk.back();
        content.binary
            = content.binary || chunk.find('\0') != std::string_view::npos;
        if (keep_lines && !content.binary) {
            std::size_t start = 0;
            while (true) {
                const std::size_t newline = chunk.find('\n', start);
                if (newline == std::string_view::npos) {
                    pending.append(chunk.substr(start));
                    break;
                }
                pending.append(chunk.substr(start, newline - start));
                if (!pending.empty() && pending.back() == '\r') {
                    pending.pop_back();
                }
                content.line_contents.push_back(std::move(pending));
                pending.clear();
                ++content.lines;
                start = newline + 1;
            }
        } else {
            content.lines
                += static_cast<std::size_t>(std::ranges::count(chunk, '\n'));
        }
    }
    content.hash = signature;
    if (!content.binary) {
        if (keep_lines) {
            if (!pending.empty()) {
                if (pending.back() == '\r') {
                    pending.pop_back();
                }
                content.line_contents.push_back(std::move(pending));
            }
            content.lines = content.line_contents.size();
        } else if (has_content && last != '\n') {
            ++content.lines;
        }
    }
    return content;
}

void summarize_untracked_files(const std::filesystem::path& root,
    const std::vector<ChangedFile>& files, ChangeSummary& summary)
{
    for (const ChangedFile& file : files) {
        if (file.kind != ChangedFile::Kind::UNTRACKED) {
            continue;
        }
        hash_bytes(summary.signature, file.path);
        summary.signature ^= 0xFF;
        summary.signature *= FNV_PRIME;
        const UntrackedFileContent content
            = read_untracked_file(root / file.path, false);
        if (!content.readable) {
            continue;
        }
        hash_bytes(summary.signature, std::to_string(content.hash));
        if (!content.binary) {
            summary.additions += content.lines;
        }
    }
}

std::optional<std::uint64_t> git_refresh_stamp(
    const std::filesystem::path& root)
{
    const std::optional<std::uint64_t> metadata = git_metadata_stamp(root);
    if (!metadata.has_value()) {
        return std::nullopt;
    }
    std::uint64_t stamp = FNV_OFFSET;
    hash_bytes(stamp, std::to_string(*metadata));
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root,
             std::filesystem::directory_options::skip_permission_denied, ec),
        end;
        !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (ec) {
                return std::nullopt;
            }
            const std::string name = it->path().filename().string();
            if (name == ".git") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto written = it->last_write_time(ec);
        if (ec) {
            return std::nullopt;
        }
        const auto size = it->file_size(ec);
        if (ec) {
            return std::nullopt;
        }
        hash_bytes(stamp, it->path().string());
        hash_bytes(stamp,
            std::to_string(
                static_cast<long long>(written.time_since_epoch().count())));
        hash_bytes(stamp, std::to_string(size));
    }
    if (ec) {
        return std::nullopt;
    }
    return stamp;
}

std::optional<std::string> git_working_diff(
    const std::filesystem::path& root, const GitDiffOptions& options)
{
    const std::string prefix = "git -C " + shell_quote(root) + " diff";
    std::string flags        = " --no-ext-diff --no-color";
    if (options.renames) {
        flags += " --find-renames --find-copies";
    }
    if (options.ignore_submodules) {
        flags += " --ignore-submodules=dirty";
    }
    if (options.numstat) {
        flags += " --numstat";
    }
    if (options.patch) {
        flags += " --patch";
    }
    // STDERR discarded: the output is parsed, and git's CRLF-conversion
    // warnings on stderr would land before the first "diff --git" header.
    CommandResult diff = run_command(prefix + flags + " HEAD --",
        std::chrono::seconds { 10 }, { }, { }, StderrCapture::STDOUT_ONLY);
    // Only a diff that ran to completion and failed is retried; a
    // timeout or spawn failure is rejected below without a retry.
    if (diff.spawned && !diff.timed_out && !command_ok(diff)) {
        diff = run_command(prefix + flags + " --cached --",
            std::chrono::seconds { 10 }, { }, { }, StderrCapture::STDOUT_ONLY);
    }
    if (!command_ok(diff)) {
        return std::nullopt;
    }
    return std::move(diff.output);
}

namespace {

    // Stdout is parsed, so git's warnings on stderr would land before the
    // value.
    std::optional<std::string> git_query(const std::filesystem::path& root,
        const std::string& arguments,
        std::chrono::seconds timeout = GIT_TIMEOUT)
    {
        const CommandResult result = run_command(git_command(root, arguments),
            timeout, { }, { }, StderrCapture::STDOUT_ONLY);
        if (!command_ok(result)) {
            return std::nullopt;
        }
        return std::string(trim(result.output));
    }

    // Short hash and subject of the new HEAD, for the commit toast.
    std::optional<std::string> git_head_summary(
        const std::filesystem::path& root)
    {
        return git_query(root, "log -1 --format='%h %s'");
    }

    // True when HEAD's branch has an upstream configured.
    bool git_has_upstream(const std::filesystem::path& root)
    {
        return git_query(
            root, "rev-parse --abbrev-ref --symbolic-full-name '@{u}'")
            .has_value();
    }

} // namespace

std::string git_command(
    const std::filesystem::path& root, std::string_view arguments)
{
    return "git -C " + shell_quote(root) + " " + std::string(arguments);
}

GitActionResult git_stage(
    const std::filesystem::path& root, std::span<const std::string> paths)
{
    if (paths.empty()) {
        return { true, { } };
    }
    return git_action(root, "add -- " + quoted_paths(paths));
}

GitActionResult git_unstage(
    const std::filesystem::path& root, std::span<const std::string> paths)
{
    if (paths.empty()) {
        return { true, { } };
    }
    // `restore --staged` diffs against HEAD, which an unborn branch lacks.
    const GitActionResult restore
        = git_action(root, "restore --staged -- " + quoted_paths(paths));
    if (restore.ok || git_query(root, "rev-parse --verify HEAD").has_value()) {
        return restore;
    }
    return git_action(root, "rm --cached -q -- " + quoted_paths(paths));
}

GitActionResult git_commit(
    const std::filesystem::path& root, std::string_view message)
{
    // The message rides stdin rather than -m: `-F -` takes the text as
    // written, so quotes, backticks and `$` inside need no shell escaping.
    // Stderr is merged so failures that only explain themselves there
    // (missing identity, signing) still reach the user; `-q` keeps the
    // success output empty.
    CommandResult result = run_command(git_command(root, "commit -q -F -"),
        GIT_TIMEOUT, { }, std::string(message));
    if (!command_ok(result)) {
        return { false, result.output };
    }
    const std::optional<std::string> summary = git_head_summary(root);
    return { true, summary.value_or("committed") };
}

GitActionResult git_push(const std::filesystem::path& root)
{
    const std::optional<std::string> branch
        = git_query(root, "branch --show-current");
    const bool has_upstream = git_has_upstream(root);
    if ((!branch.has_value() || branch->empty()) && !has_upstream) {
        return { false,
            "The current branch has no upstream and no name to "
            "publish; push it manually." };
    }
    // Quiet: git's report is a multi-line block, so the toast names the
    // branch instead.
    const GitActionResult pushed = has_upstream
        ? git_action(root, "push -q", std::chrono::seconds { 120 })
        : git_action(root, "push -q -u origin " + shell_quote(*branch),
              std::chrono::seconds { 120 });
    if (!pushed.ok) {
        return pushed;
    }
    return { true, branch.value_or("the current branch") };
}

std::optional<std::string> git_remote_origin(const std::filesystem::path& root)
{
    return git_query(root, "remote get-url origin");
}

StagedStat git_staged_stat(const std::filesystem::path& root)
{
    StagedStat stat;
    const std::optional<std::string> numstat
        = git_query(root, "diff --cached --numstat --no-renames");
    if (!numstat.has_value()) {
        return stat;
    }
    // "added<TAB>deleted<TAB>path"; binary files report "-" for both.
    const auto count = [](std::string_view field) -> std::size_t {
        std::size_t value = 0;
        std::from_chars(field.data(), field.data() + field.size(), value);
        return value;
    };
    for (const std::string& line : split_lines(*numstat)) {
        const std::size_t first_tab  = line.find('\t');
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        if (first_tab == std::string::npos || second_tab == std::string::npos) {
            continue;
        }
        ++stat.files;
        stat.additions += count(std::string_view(line).substr(0, first_tab));
        stat.deletions += count(std::string_view(line).substr(
            first_tab + 1, second_tab - first_tab - 1));
    }
    return stat;
}

} // namespace imza
