#include "tools/file_ops.h"

#include "common/util.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <system_error>

namespace imza {

namespace {

    struct EditSpan {
        std::size_t old_begin;
        std::size_t old_end;
        std::size_t new_begin;
        std::size_t new_end;
    };

    // Upper bound on the region handed to Myers: its trace costs
    // O(D*(N+M)) memory, so anything larger collapses to a single-span
    // hunk instead.
    constexpr std::size_t MAX_MYERS_LINES = 1024;

    // Myers O(ND) longest-common-subsequence; the fallback for
    // regions where patience diff finds no unique anchors.
    void myers_lcs(const std::vector<std::string>& a,
        const std::vector<std::string>& b, std::size_t a0, std::size_t a1,
        std::size_t b0, std::size_t b1, std::vector<EditSpan>& matches)
    {
        const std::size_t n = a1 - a0;
        const std::size_t m = b1 - b0;
        if (n == 0 || m == 0) {
            return;
        }
        const std::size_t max = n + m;
        const auto at
            = [&](const std::vector<std::size_t>& vv, std::ptrdiff_t k) {
                  return vv[static_cast<std::size_t>(k) + max];
              };
        std::vector<std::size_t> v(2 * max + 1, 0);
        std::vector<std::vector<std::size_t>> trace;
        std::optional<std::size_t> d_found;
        for (std::size_t d = 0; d <= max && !d_found; ++d) {
            trace.push_back(v);
            const std::ptrdiff_t dk = static_cast<std::ptrdiff_t>(d);
            for (std::ptrdiff_t k = -dk; k <= dk; k += 2) {
                std::size_t x = 0;
                if (k == -dk || (k != dk && at(v, k - 1) < at(v, k + 1))) {
                    x = at(v, k + 1);
                } else {
                    x = at(v, k - 1) + 1;
                }
                std::size_t y = static_cast<std::size_t>(
                    static_cast<std::ptrdiff_t>(x) - k);
                while (x < n && y < m && a[a0 + x] == b[b0 + y]) {
                    ++x;
                    ++y;
                }
                v[static_cast<std::size_t>(k) + max] = x;
                if (x >= n && y >= m) {
                    d_found = d;
                    break;
                }
            }
        }
        if (!d_found) {
            return;
        }
        std::size_t x = n;
        std::size_t y = m;
        std::vector<EditSpan> matched;
        for (std::size_t d = *d_found; d > 0; --d) {
            const auto& vv         = trace[d];
            const std::ptrdiff_t k = static_cast<std::ptrdiff_t>(x)
                - static_cast<std::ptrdiff_t>(y);
            std::ptrdiff_t prev_k;
            if (k == -static_cast<std::ptrdiff_t>(d)
                || (k != static_cast<std::ptrdiff_t>(d)
                    && at(vv, k - 1) < at(vv, k + 1))) {
                prev_k = k + 1;
            } else {
                prev_k = k - 1;
            }
            const std::size_t prev_x = at(vv, prev_k);
            const std::size_t prev_y = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(prev_x) - prev_k);
            while (x > prev_x && y > prev_y) {
                matched.push_back({ a0 + x - 1, a0 + x, b0 + y - 1, b0 + y });
                --x;
                --y;
            }
            x = prev_x;
            y = prev_y;
        }
        while (x > 0 && y > 0) {
            matched.push_back({ a0 + x - 1, a0 + x, b0 + y - 1, b0 + y });
            --x;
            --y;
        }
        std::reverse(matched.begin(), matched.end());
        matches.insert(matches.end(), matched.begin(), matched.end());
    }

    void patience_edits(const std::vector<std::string>& a,
        const std::vector<std::string>& b, std::size_t a0, std::size_t a1,
        std::size_t b0, std::size_t b1, std::vector<EditSpan>& edits);

    // Recurse into the gaps between LCS matches so patience anchoring
    // can localize what Myers left as one coarse region.
    void emit_from_matches(const std::vector<std::string>& a,
        const std::vector<std::string>& b, const std::vector<EditSpan>& matches,
        std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1,
        std::vector<EditSpan>& edits)
    {
        if (matches.empty()) {
            edits.push_back({ a0, a1, b0, b1 });
            return;
        }
        std::size_t oi = a0;
        std::size_t ni = b0;
        for (const EditSpan& match : matches) {
            if (match.old_begin > oi || match.new_begin > ni) {
                patience_edits(
                    a, b, oi, match.old_begin, ni, match.new_begin, edits);
            }
            oi = match.old_end;
            ni = match.new_end;
        }
        if (oi < a1 || ni < b1) {
            patience_edits(a, b, oi, a1, ni, b1, edits);
        }
    }

    // Patience diff: anchor on lines unique to both sides, keep their
    // longest increasing subsequence, recurse into the gaps between
    // anchors; anchor-free regions fall back to Myers.
    void patience_edits(const std::vector<std::string>& a,
        const std::vector<std::string>& b, std::size_t a0, std::size_t a1,
        std::size_t b0, std::size_t b1, std::vector<EditSpan>& edits)
    {
        if (a0 >= a1 || b0 >= b1) {
            if (a0 < a1 || b0 < b1) {
                edits.push_back({ a0, a1, b0, b1 });
            }
            return;
        }
        std::map<std::string, std::size_t> count;
        for (std::size_t i = a0; i < a1; ++i) {
            ++count[a[i]];
        }
        std::map<std::string, std::size_t> position;
        for (std::size_t j = b0; j < b1; ++j) {
            position.emplace(b[j], j);
        }
        // Anchors: lines appearing exactly once on each side, keyed by
        // their b position so the anchor list comes out b-ordered.
        std::map<std::string, std::size_t> b_anchor;
        for (std::size_t j = b0; j < b1; ++j) {
            auto hit = count.find(b[j]);
            if (hit == count.end() || hit->second != 1) {
                continue;
            }
            if (position[b[j]] != j) {
                continue;
            }
            b_anchor.emplace(b[j], j);
        }
        std::vector<std::pair<std::size_t, std::size_t>> anchors;
        for (std::size_t i = a0; i < a1; ++i) {
            auto it = b_anchor.find(a[i]);
            if (it != b_anchor.end()) {
                anchors.emplace_back(i, it->second);
            }
        }
        if (anchors.empty()) {
            // Trim the region's common edges first: large identical
            // heads/tails (generated files) then cost O(n), not O(ND).
            while (a0 < a1 && b0 < b1 && a[a0] == b[b0]) {
                ++a0;
                ++b0;
            }
            while (a1 > a0 && b1 > b0 && a[a1 - 1] == b[b1 - 1]) {
                --a1;
                --b1;
            }
            if (a0 >= a1 || b0 >= b1) {
                if (a0 < a1 || b0 < b1) {
                    edits.push_back({ a0, a1, b0, b1 });
                }
                return;
            }
            if (a1 - a0 + b1 - b0 > MAX_MYERS_LINES) {
                edits.push_back({ a0, a1, b0, b1 });
                return;
            }
            std::vector<EditSpan> matches;
            myers_lcs(a, b, a0, a1, b0, b1, matches);
            emit_from_matches(a, b, matches, a0, a1, b0, b1, edits);
            return;
        }
        // Longest strictly increasing subsequence over the anchor b
        // positions, with index reconstruction.
        std::vector<std::size_t> stacks;
        std::vector<std::size_t> stack_tops;
        std::vector<std::ptrdiff_t> back(anchors.size(), -1);
        for (std::size_t idx = 0; idx < anchors.size(); ++idx) {
            const std::size_t value = anchors[idx].second;
            auto it = std::lower_bound(stacks.begin(), stacks.end(), value);
            const std::size_t slot
                = static_cast<std::size_t>(it - stacks.begin());
            if (it == stacks.end()) {
                stacks.push_back(value);
                stack_tops.push_back(idx);
            } else {
                *it              = value;
                stack_tops[slot] = idx;
            }
            back[idx] = slot == 0
                ? -1
                : static_cast<std::ptrdiff_t>(stack_tops[slot - 1]);
        }
        std::vector<std::pair<std::size_t, std::size_t>> chain;
        for (std::ptrdiff_t idx
            = static_cast<std::ptrdiff_t>(stack_tops.back());
            idx >= 0; idx = back[static_cast<std::size_t>(idx)]) {
            chain.push_back(anchors[static_cast<std::size_t>(idx)]);
        }
        std::reverse(chain.begin(), chain.end());
        std::size_t oi = a0;
        std::size_t ni = b0;
        for (const auto& [ai, bi] : chain) {
            if (ai > oi || bi > ni) {
                patience_edits(a, b, oi, ai, ni, bi, edits);
            }
            oi = ai + 1;
            ni = bi + 1;
        }
        if (oi < a1 || ni < b1) {
            patience_edits(a, b, oi, a1, ni, b1, edits);
        }
    }

    std::vector<EditSpan> compute_edits(
        const std::vector<std::string>& old_lines,
        const std::vector<std::string>& new_lines)
    {
        std::vector<EditSpan> edits;
        patience_edits(old_lines, new_lines, 0, old_lines.size(), 0,
            new_lines.size(), edits);
        return edits;
    }

    DiffView build_diff_view(const std::string& path,
        const std::vector<std::string>& old_lines,
        const std::vector<std::string>& new_lines,
        const std::vector<EditSpan>& edits, std::size_t context = 3)
    {
        DiffView dv;
        dv.file = path;
        if (edits.empty()) {
            return dv;
        }
        const auto push_same
            = [&](std::size_t o0, std::size_t o1, std::size_t n0) {
                  for (std::size_t k = o0; k < o1; ++k) {
                      DiffRow r;
                      r.kind              = DiffRow::Kind::SAME;
                      r.left_no           = k + 1;
                      const std::size_t n = n0 + (k - o0);
                      r.right_no          = n + 1;
                      r.left              = old_lines[k];
                      r.right             = new_lines[n];
                      dv.rows.push_back(std::move(r));
                  }
              };
        const auto push_skip
            = [&](std::size_t count, std::size_t lo, std::size_t ln) {
                  DiffRow r;
                  r.kind     = DiffRow::Kind::SKIP;
                  r.left_no  = lo;
                  r.right_no = ln;
                  r.left     = "\xE2\x80\xA6 " + std::to_string(count)
                      + " unchanged line(s) \xE2\x80\xA6";
                  r.right = r.left;
                  dv.rows.push_back(std::move(r));
              };
        const auto push_edit = [&](const EditSpan& ed) {
            for (std::size_t k = ed.old_begin; k < ed.old_end; ++k) {
                DiffRow r;
                r.kind    = DiffRow::Kind::REMOVE;
                r.left_no = k + 1;
                r.left    = old_lines[k];
                dv.rows.push_back(std::move(r));
            }
            for (std::size_t k = ed.new_begin; k < ed.new_end; ++k) {
                DiffRow r;
                r.kind     = DiffRow::Kind::ADD;
                r.right_no = k + 1;
                r.right    = new_lines[k];
                dv.rows.push_back(std::move(r));
            }
        };

        // Git-style hunk grouping: fewer than 2*context unchanged
        // lines between edits render in full, merging the hunks; larger
        // gaps keep context on both sides and elide the middle.
        std::size_t oi = 0;
        std::size_t ni = 0;
        for (const EditSpan& ed : edits) {
            const std::size_t gap_len = ed.old_begin - oi;
            if (gap_len > 0) {
                if (gap_len >= 2 * context) {
                    push_same(oi, oi + context, ni);
                    push_skip(gap_len - 2 * context, oi + context + 1,
                        ni + context + 1);
                    push_same(ed.old_begin - context, ed.old_begin,
                        ni + (ed.old_begin - context - oi));
                } else {
                    push_same(oi, ed.old_begin, ni);
                }
            }
            push_edit(ed);
            oi = ed.old_end;
            ni = ed.new_end;
        }
        const std::size_t tail_len = old_lines.size() - oi;
        if (tail_len > 0) {
            const std::size_t take = std::min(tail_len, context);
            push_same(oi, oi + take, ni);
            if (tail_len > context) {
                push_skip(tail_len - context, oi + take + 1, ni + take + 1);
            }
        }
        return dv;
    }

} // namespace

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
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        err = "cannot open: " + path;
        return false;
    }
    const std::string content(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (content.find('\0') != std::string::npos) {
        err = "binary file: " + path;
        return false;
    }
    out = content;
    return true;
}

bool save_text(
    const std::string& path, const std::string& content, std::string& err)
{
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

std::optional<std::string> insert_text(const std::string& content,
    const std::string& text, std::size_t line, std::string& err)
{
    const bool trailing_newline    = content.empty() || content.back() == '\n';
    std::vector<std::string> lines = split_lines(content);
    if (line > lines.size() + 1) {
        err = "line " + std::to_string(line) + " exceeds file length "
            + std::to_string(lines.size())
            + " (line is 1-based; omit it to append at the end)";
        return std::nullopt;
    }
    const std::vector<std::string> insert = split_lines(text);
    const std::size_t at                  = line == 0 ? lines.size() : line - 1;
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
        insert.begin(), insert.end());
    return join_lines(lines, trailing_newline);
}

std::optional<std::string> replace_text(const std::string& content,
    const std::string& old, const std::string& fresh, std::size_t count,
    std::string& err)
{
    if (old.empty()) {
        err = "old must be a non-empty string";
        return std::nullopt;
    }
    std::vector<std::size_t> matches;
    std::size_t p = 0;
    while ((p = content.find(old, p)) != std::string::npos) {
        matches.push_back(p);
        p += old.size();
    }
    if (matches.empty()) {
        std::string_view shown(old);
        if (shown.size() > 40) {
            shown = shown.substr(0, 40);
        }
        std::string quoted;
        for (const char c : shown) {
            if (c == '\n') {
                quoted += "\\n";
            } else if (c == '\t') {
                quoted += "\\t";
            } else {
                quoted += c;
            }
        }
        err = "old text not found in file: \"" + quoted
            + (old.size() > 40 ? "..." : "")
            + "\"; it must match exactly, including whitespace";
        return std::nullopt;
    }
    const std::size_t n
        = count == 0 ? matches.size() : std::min(count, matches.size());
    std::string out;
    out.reserve(content.size() + n * fresh.size());
    std::size_t cursor = 0;
    for (std::size_t idx = 0; idx < n; ++idx) {
        const std::size_t match = matches[idx];
        out.append(content, cursor, match - cursor);
        out += fresh;
        cursor = match + old.size();
    }
    out += content.substr(cursor);
    return out;
}

DiffView make_diff_view(const std::string& path,
    const std::vector<std::string>& old_lines,
    const std::vector<std::string>& new_lines)
{
    return build_diff_view(
        path, old_lines, new_lines, compute_edits(old_lines, new_lines));
}

} // namespace imza
