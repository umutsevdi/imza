#include "tools/file_ops.h"

#include "common/util.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace imza {

namespace {

    struct EditSpan {
        std::size_t old_begin;
        std::size_t old_end;
        std::size_t new_begin;
        std::size_t new_end;
    };

    DiffView build_diff_view(const std::string& path,
        const std::vector<std::string>& old_lines,
        const std::vector<std::string>& new_lines,
        const std::vector<EditSpan>& edits, std::size_t context = 3)
    {
        DiffView dv;
        dv.file = path;
        if (old_lines.empty() && new_lines.empty()) {
            return dv;
        }
        const auto push_same
            = [&](std::size_t o0, std::size_t o1, std::size_t n0) {
                  const std::size_t len = o1 - o0;
                  for (std::size_t k = 0; k < len; ++k) {
                      DiffRow r;
                      r.kind     = DiffRow::Kind::SAME;
                      r.left_no  = o0 + k + 1;
                      r.right_no = n0 + k + 1;
                      r.left     = old_lines[o0 + k];
                      r.right    = new_lines[n0 + k];
                      dv.rows.push_back(std::move(r));
                  }
              };
        const auto push_skip = [&](std::size_t count, std::size_t lo,
                                   std::size_t ln) {
            DiffRow r;
            r.kind     = DiffRow::Kind::SAME;
            r.left_no  = lo;
            r.right_no = ln;
            r.left     = "… " + std::to_string(count) + " unchanged line(s) …";
            r.right    = r.left;
            dv.rows.push_back(std::move(r));
        };
        const auto push_edit = [&](const EditSpan& ed) {
            const std::size_t olen = ed.old_end - ed.old_begin;
            const std::size_t nlen = ed.new_end - ed.new_begin;
            const std::size_t len  = std::max(olen, nlen);
            for (std::size_t k = 0; k < len; ++k) {
                DiffRow r;
                r.kind = (k < olen && k >= nlen) ? DiffRow::Kind::REMOVE
                                                 : DiffRow::Kind::ADD;
                if (k < olen) {
                    r.left_no = ed.old_begin + k + 1;
                    r.left    = old_lines[ed.old_begin + k];
                }
                if (k < nlen) {
                    r.right_no = ed.new_begin + k + 1;
                    r.right    = new_lines[ed.new_begin + k];
                }
                dv.rows.push_back(std::move(r));
            }
        };

        std::size_t oi = 0;
        std::size_t ni = 0;
        for (std::size_t e = 0; e < edits.size(); ++e) {
            const EditSpan& ed        = edits[e];
            const std::size_t gap_len = ed.old_begin - oi;
            if (gap_len > 0) {
                const std::size_t take  = std::min(gap_len, context);
                const std::size_t start = ed.old_begin - take;
                if (gap_len > context) {
                    push_skip(gap_len - context, oi + 1, ni + 1);
                }
                push_same(start, ed.old_begin, ni + (start - oi));
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
            + std::to_string(lines.size());
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
        err = "old string not found";
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
    std::size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size()
        && old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }
    std::size_t old_suffix = old_lines.size();
    std::size_t new_suffix = new_lines.size();
    while (old_suffix > prefix && new_suffix > prefix
        && old_lines[old_suffix - 1] == new_lines[new_suffix - 1]) {
        --old_suffix;
        --new_suffix;
    }
    std::vector<EditSpan> edits;
    if (prefix != old_lines.size() || prefix != new_lines.size()) {
        edits.push_back({ prefix, old_suffix, prefix, new_suffix });
    }
    return build_diff_view(path, old_lines, new_lines, edits);
}

} // namespace imza
