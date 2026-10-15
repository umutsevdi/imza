#pragma once

#include <tree_sitter/api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace imza {

// RAII ownership for tree-sitter objects and the query-predicate step
// decoder shared by syntax highlighting and the ts_query Lua binding.

struct TsQueryDeleter {
    void operator()(TSQuery* query) const { ts_query_delete(query); }
};

struct TsParserDeleter {
    void operator()(TSParser* parser) const { ts_parser_delete(parser); }
};

struct TsTreeDeleter {
    void operator()(TSTree* tree) const { ts_tree_delete(tree); }
};

struct TsQueryCursorDeleter {
    void operator()(TSQueryCursor* cursor) const
    {
        ts_query_cursor_delete(cursor);
    }
};

using TsQueryPtr       = std::unique_ptr<TSQuery, TsQueryDeleter>;
using TsParserPtr      = std::unique_ptr<TSParser, TsParserDeleter>;
using TsTreePtr        = std::unique_ptr<TSTree, TsTreeDeleter>;
using TsQueryCursorPtr = std::unique_ptr<TSQueryCursor, TsQueryCursorDeleter>;

// One predicate argument: a capture reference or a string literal.
struct TsPredicateStep {
    bool is_capture        = false;
    std::uint32_t value_id = 0;
};

using TsPredicateGroup = std::vector<TsPredicateStep>;

inline std::string ts_string_value(const TSQuery* query, std::uint32_t value_id)
{
    std::uint32_t length = 0;
    const char* value = ts_query_string_value_for_id(query, value_id, &length);
    return std::string(value, length);
}

// The predicate groups declared for one query pattern, each a #op?
// followed by its arguments, with the Done separators removed. An empty
// trailing group without its separator is still emitted so callers keep
// their own defensive handling.
inline std::vector<TsPredicateGroup> ts_predicate_groups(
    const TSQuery* query, std::uint32_t pattern_index)
{
    std::uint32_t step_count = 0;
    const TSQueryPredicateStep* steps
        = ts_query_predicates_for_pattern(query, pattern_index, &step_count);
    std::vector<TsPredicateGroup> groups;
    TsPredicateGroup current;
    for (std::uint32_t i = 0; i < step_count; ++i) {
        if (steps[i].type == TSQueryPredicateStepTypeDone) {
            groups.push_back(std::move(current));
            continue;
        }
        current.push_back({ steps[i].type == TSQueryPredicateStepTypeCapture,
            steps[i].value_id });
    }
    if (!current.empty()) {
        groups.push_back(std::move(current));
    }
    return groups;
}

} // namespace imza
