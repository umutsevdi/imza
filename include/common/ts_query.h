#pragma once

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace imza {

// RAII ownership for tree-sitter objects and the query-predicate engine
// shared by syntax highlighting and the ts_query Lua binding.

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

// Translates the Lua-pattern subset used by `#lua-match?` (%u, %l, %d, %w)
// into the equivalent ECMAScript character classes.
inline std::string ts_lua_regex(std::string_view pattern)
{
    std::string converted;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%' || i + 1 == pattern.size()) {
            converted.push_back(pattern[i]);
            continue;
        }
        switch (pattern[++i]) {
        case 'u': converted += "A-Z"; break;
        case 'l': converted += "a-z"; break;
        case 'd': converted += "0-9"; break;
        case 'w': converted += "A-Za-z0-9_"; break;
        default: converted.push_back(pattern[i]); break;
        }
    }
    return converted;
}

// One compiled predicate of a query pattern. `capture` is the resolved
// capture name (a predicate step carries the name id, not the capture
// index); `field` is the ".suffix" form's field name, empty when the
// predicate reads the capture's own text.
struct TsPredicate {
    enum class Kind {
        EQUAL,
        NOT_EQUAL,
        ANY_OF,
        NOT_ANY_OF,
        MATCH,
        NOT_MATCH,
    };

    Kind kind = Kind::EQUAL;
    std::string capture;
    std::string field;
    std::vector<std::string> values;
    std::optional<std::regex> regex;
};

// Predicates per pattern index; patterns without predicates stay empty.
using TsPredicateSet = std::vector<std::vector<TsPredicate>>;

namespace ts_detail {

    inline std::string capture_name(const TSQuery* query, std::uint32_t id)
    {
        std::uint32_t length = 0;
        const char* name     = ts_query_capture_name_for_id(query, id, &length);
        return std::string(name, length);
    }

    inline std::string_view node_text(std::string_view code, const TSNode& node)
    {
        const std::uint32_t begin = ts_node_start_byte(node);
        const std::uint32_t end   = ts_node_end_byte(node);
        if (begin > end || end > code.size()) {
            return { };
        }
        return code.substr(begin, end - begin);
    }

    // The text of `field` on `node`'s children, or empty when no child
    // carries that field.
    inline std::string_view field_text(
        std::string_view code, const TSNode& node, std::string_view field)
    {
        const std::uint32_t children = ts_node_child_count(node);
        for (std::uint32_t child = 0; child < children; ++child) {
            const char* name = ts_node_field_name_for_child(node, child);
            if (name != nullptr && field == name) {
                const TSNode found = ts_node_child(node, child);
                if (!ts_node_is_null(found)) {
                    return node_text(code, found);
                }
            }
        }
        return { };
    }

    // The text a predicate argument binds: the named capture's own text,
    // or the field named by its ".suffix" form (e.g. @function.kind).
    inline std::string_view predicate_text(const TSQuery* query,
        const TSQueryMatch& match, std::string_view code,
        std::string_view capture, std::string_view field)
    {
        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& captured = match.captures[i];
            if (capture_name(query, captured.index) != capture) {
                continue;
            }
            return field.empty() ? node_text(code, captured.node)
                                 : field_text(code, captured.node, field);
        }
        return { };
    }

} // namespace ts_detail

// Compiles every pattern's predicates once, so match-time evaluation is a
// table lookup instead of re-decoding the query. Unknown predicates are
// dropped (they never filter); an unusable regex drops its predicate
// rather than silently discarding matches.
inline TsPredicateSet ts_compile_predicates(const TSQuery* query)
{
    TsPredicateSet predicates(ts_query_pattern_count(query));
    for (std::uint32_t pattern = 0; pattern < predicates.size(); ++pattern) {
        for (const TsPredicateGroup& group :
            ts_predicate_groups(query, pattern)) {
            if (group.empty() || group.front().is_capture) {
                continue;
            }
            std::string operation
                = ts_string_value(query, group.front().value_id);
            if (operation.starts_with('#')) {
                operation.erase(0, 1);
            }
            std::optional<TsPredicate::Kind> kind;
            if (operation == "eq?") {
                kind = TsPredicate::Kind::EQUAL;
            } else if (operation == "not-eq?") {
                kind = TsPredicate::Kind::NOT_EQUAL;
            } else if (operation == "any-of?") {
                kind = TsPredicate::Kind::ANY_OF;
            } else if (operation == "not-any-of?") {
                kind = TsPredicate::Kind::NOT_ANY_OF;
            } else if (operation == "match?" || operation == "lua-match?") {
                kind = TsPredicate::Kind::MATCH;
            } else if (operation == "not-match?") {
                kind = TsPredicate::Kind::NOT_MATCH;
            }
            if (!kind.has_value()) {
                continue;
            }

            std::optional<std::uint32_t> capture_id;
            std::vector<std::string> values;
            for (std::size_t at = 1; at < group.size(); ++at) {
                if (group[at].is_capture && !capture_id.has_value()) {
                    capture_id = group[at].value_id;
                } else if (!group[at].is_capture) {
                    values.push_back(
                        ts_string_value(query, group[at].value_id));
                }
            }
            if (!capture_id.has_value() || values.empty()) {
                continue;
            }

            TsPredicate predicate;
            predicate.kind        = *kind;
            predicate.capture     = ts_detail::capture_name(query, *capture_id);
            const std::size_t dot = predicate.capture.find('.');
            if (dot != std::string::npos) {
                predicate.field = predicate.capture.substr(dot + 1);
                predicate.capture.erase(dot);
            }
            predicate.values = std::move(values);
            if (predicate.kind == TsPredicate::Kind::MATCH
                || predicate.kind == TsPredicate::Kind::NOT_MATCH) {
                const std::string pattern_text = operation == "lua-match?"
                    ? ts_lua_regex(predicate.values.front())
                    : predicate.values.front();
                try {
                    predicate.regex.emplace(pattern_text);
                } catch (const std::regex_error&) {
                    continue; // unusable pattern: never filter on it
                }
            }
            predicates[pattern].push_back(std::move(predicate));
        }
    }
    return predicates;
}

// True when every predicate of the match's pattern holds. A pattern index
// past the compiled set filters nothing.
inline bool ts_predicates_hold(const TsPredicateSet& predicates,
    const TSQuery* query, const TSQueryMatch& match, std::string_view code)
{
    if (match.pattern_index >= predicates.size()) {
        return true;
    }
    for (const TsPredicate& predicate : predicates[match.pattern_index]) {
        const std::string_view body = ts_detail::predicate_text(
            query, match, code, predicate.capture, predicate.field);
        bool matched = true;
        switch (predicate.kind) {
        case TsPredicate::Kind::EQUAL:
        case TsPredicate::Kind::NOT_EQUAL:
            matched = body == predicate.values.front();
            break;
        case TsPredicate::Kind::ANY_OF:
        case TsPredicate::Kind::NOT_ANY_OF:
            matched = std::ranges::find(predicate.values, body)
                != predicate.values.end();
            break;
        case TsPredicate::Kind::MATCH:
        case TsPredicate::Kind::NOT_MATCH:
            matched = predicate.regex.has_value()
                && std::regex_search(
                    body.begin(), body.end(), *predicate.regex);
            break;
        }
        if (predicate.kind == TsPredicate::Kind::NOT_EQUAL
            || predicate.kind == TsPredicate::Kind::NOT_ANY_OF
            || predicate.kind == TsPredicate::Kind::NOT_MATCH) {
            matched = !matched;
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

} // namespace imza
