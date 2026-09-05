#include "core/git.h"
#include "ui/ui.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ursa {
namespace {

    using namespace ftxui;

    enum class HighlightSide { OLD, NEW };

    void cache_document(ReviewHighlights& cache,
        const std::vector<std::pair<const ReviewLine*, std::string>>& lines,
        std::string_view syntax, HighlightSide side)
    {
        if (lines.empty()) {
            return;
        }
        std::size_t size = lines.size() - 1;
        for (const auto& [line, content] : lines) {
            static_cast<void>(line);
            size += content.size();
        }
        std::string code;
        code.reserve(size);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i != 0) {
                code.push_back('\n');
            }
            code += lines[i].second;
        }
        Elements highlighted    = highlight_code(code, syntax);
        const std::size_t count = std::min(lines.size(), highlighted.size());
        for (std::size_t i = 0; i < count; ++i) {
            ReviewLineHighlights& target = cache[lines[i].first];
            if (side == HighlightSide::OLD) {
                target.old_side = std::move(highlighted[i]);
            } else {
                target.new_side = std::move(highlighted[i]);
            }
        }
    }

} // namespace

void append_review_hunk_highlights(ReviewHighlights& cache,
    const ReviewHunk& hunk, std::string_view path, int review_width,
    int horizontal_offset, bool side_by_side)
{
    const bool cached
        = std::ranges::all_of(hunk.lines, [&cache](const auto& line) {
              if (line.kind == ReviewLine::Kind::META) {
                  return true;
              }
              const auto found = cache.find(&line);
              if (found == cache.end()) {
                  return false;
              }
              return line.kind == ReviewLine::Kind::ADDITION
                  ? found->second.new_side != nullptr
                  : line.kind == ReviewLine::Kind::DELETION
                  ? found->second.old_side != nullptr
                  : found->second.old_side != nullptr
                      && found->second.new_side != nullptr;
          });
    if (cached) {
        return;
    }
    const int content_width  = side_by_side
        ? std::max(1, std::max(20, (review_width - 3) / 2) - 8)
        : std::max(1, review_width - 14);
    const std::string syntax = syntax_type_for_path(path);
    std::vector<std::pair<const ReviewLine*, std::string>> old;
    std::vector<std::pair<const ReviewLine*, std::string>> next;
    old.reserve(hunk.lines.size());
    next.reserve(hunk.lines.size());
    for (const ReviewLine& line : hunk.lines) {
        if (line.kind == ReviewLine::Kind::META) {
            continue;
        }
        std::string content
            = fit(line.content, content_width, horizontal_offset);
        if (line.kind != ReviewLine::Kind::ADDITION) {
            old.emplace_back(&line, content);
        }
        if (line.kind != ReviewLine::Kind::DELETION) {
            next.emplace_back(&line, std::move(content));
        }
    }
    cache_document(cache, old, syntax, HighlightSide::OLD);
    cache_document(cache, next, syntax, HighlightSide::NEW);
}

} // namespace ursa
