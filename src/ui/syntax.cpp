#include "ui/ui.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    enum class SyntaxStyle {
        PLAIN,
        KEYWORD,
        TYPE,
        STRING,
        NUMBER,
        COMMENT,
        SPECIAL,
    };

    struct QueryDeleter {
        void operator()(TSQuery* query) const { ts_query_delete(query); }
    };

    struct ParserDeleter {
        void operator()(TSParser* parser) const { ts_parser_delete(parser); }
    };

    struct TreeDeleter {
        void operator()(TSTree* tree) const { ts_tree_delete(tree); }
    };

    struct CursorDeleter {
        void operator()(TSQueryCursor* cursor) const
        {
            ts_query_cursor_delete(cursor);
        }
    };

    using QueryPtr  = std::unique_ptr<TSQuery, QueryDeleter>;
    using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
    using TreePtr   = std::unique_ptr<TSTree, TreeDeleter>;
    using CursorPtr = std::unique_ptr<TSQueryCursor, CursorDeleter>;

    using LanguageFunction = const TSLanguage* (*)();

    struct LanguageSpec {
        std::string_view name;
        std::span<const std::string_view> extensions;
        std::span<const std::string_view> filenames;
        LanguageFunction load_language { };
        std::string_view highlight_query;
    };

#include "syntax_registry.inc"

    struct QueryPredicate {
        enum class Kind {
            EQUAL,
            NOT_EQUAL,
            ANY_OF,
            NOT_ANY_OF,
            MATCH,
            NOT_MATCH,
            INVALID,
        };

        Kind kind           = Kind::INVALID;
        uint32_t capture_id = 0;
        std::vector<std::string> values;
        std::optional<std::regex> regex;
    };

    struct LanguageDefinition {
        std::string_view name;
        std::span<const std::string_view> extensions;
        std::span<const std::string_view> filenames;
        LanguageFunction load_language { };
        std::string_view highlight_query;
        struct Runtime {
            std::once_flag initialized;
            const TSLanguage* language { };
            QueryPtr query;
            ParserPtr parser;
            CursorPtr cursor;
            std::vector<std::vector<QueryPredicate>> predicates;
            std::mutex use_mutex;
        };
        std::unique_ptr<Runtime> runtime;
    };

    std::string lower(std::string_view value)
    {
        std::string out(value);
        std::ranges::transform(out, out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    QueryPtr make_query(const TSLanguage* language, std::string_view source)
    {
        uint32_t offset    = 0;
        TSQueryError error = TSQueryErrorNone;
        return QueryPtr(ts_query_new(language, source.data(),
            static_cast<uint32_t>(source.size()), &offset, &error));
    }

    std::vector<LanguageDefinition> make_languages()
    {
        std::vector<LanguageDefinition> languages;
        languages.reserve(LANGUAGE_SPECS.size());
        for (const LanguageSpec& spec : LANGUAGE_SPECS) {
            languages.push_back(LanguageDefinition { spec.name, spec.extensions,
                spec.filenames, spec.load_language, spec.highlight_query,
                std::make_unique<LanguageDefinition::Runtime>() });
        }
        return languages;
    }

    const std::vector<LanguageDefinition>& languages()
    {
        static const std::vector<LanguageDefinition> registry
            = make_languages();
        return registry;
    }

    bool contains(
        std::span<const std::string_view> values, std::string_view candidate)
    {
        return std::ranges::find(values, candidate) != values.end();
    }

    const LanguageDefinition* language_for_type(std::string_view hint)
    {
        const std::size_t end = hint.find_first_of(" \t{");
        std::string type      = lower(hint.substr(0, end));
        if (!type.empty() && type.front() == '.') {
            type.erase(0, 1);
        }
        const auto& registry = languages();
        const auto language
            = std::ranges::find_if(registry, [&type](const auto& candidate) {
                  return type == candidate.name
                      || contains(candidate.extensions, type);
              });
        return language == registry.end() ? nullptr : &*language;
    }

    const LanguageDefinition* language_for_path(std::string_view path)
    {
        const std::filesystem::path file(path);
        std::string extension = lower(file.extension().string());
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(0, 1);
        }
        const std::string filename   = lower(file.filename().string());
        const auto& registry         = languages();
        const auto filename_language = std::ranges::find_if(
            registry, [&filename](const auto& candidate) {
                return contains(candidate.filenames, filename);
            });
        if (filename_language != registry.end()) {
            return &*filename_language;
        }
        const auto extension_language = std::ranges::find_if(
            registry, [&extension](const auto& candidate) {
                return contains(candidate.extensions, extension);
            });
        return extension_language == registry.end() ? nullptr
                                                    : &*extension_language;
    }

    SyntaxStyle capture_style(std::string_view capture)
    {
        if (capture.starts_with("comment")) {
            return SyntaxStyle::COMMENT;
        }
        if (capture.starts_with("string") || capture.starts_with("character")) {
            return SyntaxStyle::STRING;
        }
        if (capture.starts_with("number") || capture.starts_with("float")
            || capture.starts_with("constant") || capture == "boolean") {
            return SyntaxStyle::NUMBER;
        }
        if (capture.starts_with("type") || capture.starts_with("tag")
            || capture.starts_with("attribute")
            || capture == "variable.builtin") {
            return SyntaxStyle::TYPE;
        }
        if (capture.starts_with("keyword") || capture.starts_with("operator")
            || capture.starts_with("conditional")
            || capture.starts_with("repeat") || capture.starts_with("include")
            || capture.starts_with("exception")) {
            return SyntaxStyle::KEYWORD;
        }
        if (capture.starts_with("preproc") || capture.starts_with("function")
            || capture.starts_with("method")
            || capture.starts_with("constructor")
            || capture.starts_with("label") || capture.starts_with("escape")
            || capture == "module") {
            return SyntaxStyle::SPECIAL;
        }
        return SyntaxStyle::PLAIN;
    }

    Color style_color(SyntaxStyle style)
    {
        switch (style) {
        case SyntaxStyle::KEYWORD: return Color::Green;
        case SyntaxStyle::TYPE: return Color::Cyan;
        case SyntaxStyle::STRING: return Color::Yellow;
        case SyntaxStyle::NUMBER: return Color::Magenta;
        case SyntaxStyle::COMMENT: return Color::GrayLight;
        case SyntaxStyle::SPECIAL: return Color::Blue;
        case SyntaxStyle::PLAIN: return PANEL_FG;
        }
        return PANEL_FG;
    }

    std::string_view capture_text(
        const TSQueryMatch& match, uint32_t capture_id, std::string_view code)
    {
        for (uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];
            if (capture.index != capture_id) {
                continue;
            }
            const uint32_t begin = ts_node_start_byte(capture.node);
            const uint32_t end   = ts_node_end_byte(capture.node);
            if (begin <= end && end <= code.size()) {
                return code.substr(begin, end - begin);
            }
        }
        return { };
    }

    std::string lua_regex(std::string_view pattern)
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

    std::vector<std::vector<QueryPredicate>> compile_predicates(
        const TSQuery* query)
    {
        std::vector<std::vector<QueryPredicate>> predicates(
            ts_query_pattern_count(query));
        for (uint32_t pattern = 0; pattern < predicates.size(); ++pattern) {
            uint32_t count = 0;
            const TSQueryPredicateStep* steps
                = ts_query_predicates_for_pattern(query, pattern, &count);
            uint32_t at = 0;
            while (at < count) {
                if (steps[at].type != TSQueryPredicateStepTypeString) {
                    while (at < count
                        && steps[at].type != TSQueryPredicateStepTypeDone) {
                        ++at;
                    }
                    ++at;
                    continue;
                }
                uint32_t operation_size   = 0;
                const char* operation_raw = ts_query_string_value_for_id(
                    query, steps[at++].value_id, &operation_size);
                std::string_view operation(operation_raw, operation_size);
                if (operation.starts_with('#')) {
                    operation.remove_prefix(1);
                }
                std::optional<uint32_t> capture_id;
                std::vector<std::string> values;
                while (at < count
                    && steps[at].type != TSQueryPredicateStepTypeDone) {
                    if (steps[at].type == TSQueryPredicateStepTypeCapture
                        && !capture_id.has_value()) {
                        capture_id = steps[at].value_id;
                    } else if (steps[at].type
                        == TSQueryPredicateStepTypeString) {
                        uint32_t value_size   = 0;
                        const char* value_raw = ts_query_string_value_for_id(
                            query, steps[at].value_id, &value_size);
                        values.emplace_back(value_raw, value_size);
                    }
                    ++at;
                }
                ++at;
                if (!capture_id.has_value() || values.empty()) {
                    continue;
                }

                std::optional<QueryPredicate::Kind> kind;
                if (operation == "eq?") {
                    kind = QueryPredicate::Kind::EQUAL;
                } else if (operation == "not-eq?") {
                    kind = QueryPredicate::Kind::NOT_EQUAL;
                } else if (operation == "any-of?") {
                    kind = QueryPredicate::Kind::ANY_OF;
                } else if (operation == "not-any-of?") {
                    kind = QueryPredicate::Kind::NOT_ANY_OF;
                } else if (operation == "match?" || operation == "lua-match?") {
                    kind = QueryPredicate::Kind::MATCH;
                } else if (operation == "not-match?") {
                    kind = QueryPredicate::Kind::NOT_MATCH;
                }
                if (!kind.has_value()) {
                    continue;
                }

                std::optional<std::regex> regex;
                if (*kind == QueryPredicate::Kind::MATCH
                    || *kind == QueryPredicate::Kind::NOT_MATCH) {
                    try {
                        regex.emplace(operation == "lua-match?"
                                ? lua_regex(values.front())
                                : values.front());
                    } catch (const std::regex_error&) {
                        kind = QueryPredicate::Kind::INVALID;
                    }
                }
                predicates[pattern].push_back(QueryPredicate {
                    *kind, *capture_id, std::move(values), std::move(regex) });
            }
        }
        return predicates;
    }

    LanguageDefinition::Runtime& language_runtime(
        const LanguageDefinition& definition)
    {
        LanguageDefinition::Runtime& runtime = *definition.runtime;
        std::call_once(runtime.initialized, [&definition, &runtime] {
            runtime.language = definition.load_language();
            if (runtime.language == nullptr) {
                return;
            }
            runtime.query
                = make_query(runtime.language, definition.highlight_query);
            if (runtime.query != nullptr) {
                runtime.predicates = compile_predicates(runtime.query.get());
            }
            runtime.parser.reset(ts_parser_new());
            if (runtime.parser != nullptr
                && !ts_parser_set_language(
                    runtime.parser.get(), runtime.language)) {
                runtime.parser.reset();
            }
            runtime.cursor.reset(ts_query_cursor_new());
        });
        return runtime;
    }

    bool predicates_match(const LanguageDefinition::Runtime& runtime,
        const TSQueryMatch& match, std::string_view code)
    {
        if (match.pattern_index >= runtime.predicates.size()) {
            return true;
        }
        for (const QueryPredicate& predicate :
            runtime.predicates[match.pattern_index]) {
            const std::string_view body
                = capture_text(match, predicate.capture_id, code);
            bool matched = true;
            switch (predicate.kind) {
            case QueryPredicate::Kind::EQUAL:
            case QueryPredicate::Kind::NOT_EQUAL:
                matched = body == predicate.values.front();
                break;
            case QueryPredicate::Kind::ANY_OF:
            case QueryPredicate::Kind::NOT_ANY_OF:
                matched = std::ranges::find(predicate.values, body)
                    != predicate.values.end();
                break;
            case QueryPredicate::Kind::MATCH:
            case QueryPredicate::Kind::NOT_MATCH:
                matched = predicate.regex.has_value()
                    && std::regex_search(
                        body.begin(), body.end(), *predicate.regex);
                break;
            case QueryPredicate::Kind::INVALID: matched = false; break;
            }
            if (predicate.kind == QueryPredicate::Kind::NOT_EQUAL
                || predicate.kind == QueryPredicate::Kind::NOT_ANY_OF
                || predicate.kind == QueryPredicate::Kind::NOT_MATCH) {
                matched = !matched;
            }
            if (!matched) {
                return false;
            }
        }
        return true;
    }

    std::vector<SyntaxStyle> syntax_styles(
        std::string_view code, const LanguageDefinition& language)
    {
        std::vector<SyntaxStyle> styles(code.size(), SyntaxStyle::PLAIN);
        if (code.empty()
            || code.size() > std::numeric_limits<uint32_t>::max()) {
            return styles;
        }
        LanguageDefinition::Runtime& runtime = language_runtime(language);
        const TSQuery* query                 = runtime.query.get();
        if (runtime.language == nullptr || query == nullptr
            || runtime.parser == nullptr || runtime.cursor == nullptr) {
            return styles;
        }
        std::lock_guard lock(runtime.use_mutex);
        TreePtr tree(ts_parser_parse_string(runtime.parser.get(), nullptr,
            code.data(), static_cast<uint32_t>(code.size())));
        if (!tree) {
            return styles;
        }
        ts_query_cursor_exec(
            runtime.cursor.get(), query, ts_tree_root_node(tree.get()));

        std::vector<uint32_t> span_sizes(
            code.size(), std::numeric_limits<uint32_t>::max());
        TSQueryMatch match { };
        uint32_t capture_index = 0;
        while (ts_query_cursor_next_capture(
            runtime.cursor.get(), &match, &capture_index)) {
            if (!predicates_match(runtime, match, code)) {
                continue;
            }
            const TSQueryCapture& capture = match.captures[capture_index];
            const uint32_t begin          = ts_node_start_byte(capture.node);
            const uint32_t end            = ts_node_end_byte(capture.node);
            if (begin >= end || end > code.size()) {
                continue;
            }
            uint32_t name_size = 0;
            const char* name   = ts_query_capture_name_for_id(
                query, capture.index, &name_size);
            const SyntaxStyle style
                = capture_style(std::string_view(name, name_size));
            if (style == SyntaxStyle::PLAIN) {
                continue;
            }
            const uint32_t span_size = end - begin;
            for (uint32_t i = begin; i < end; ++i) {
                if (span_size <= span_sizes[i]) {
                    styles[i]     = style;
                    span_sizes[i] = span_size;
                }
            }
        }
        return styles;
    }

    Element render_line(std::string_view code,
        const std::vector<SyntaxStyle>& styles, std::size_t begin,
        std::size_t end)
    {
        Elements spans;
        std::size_t at = begin;
        while (at < end) {
            const SyntaxStyle style = styles[at];
            std::size_t next        = at + 1;
            while (next < end && styles[next] == style) {
                ++next;
            }
            spans.push_back(text(std::string(code.substr(at, next - at)))
                | color(style_color(style)));
            at = next;
        }
        if (spans.empty()) {
            return text("") | color(PANEL_FG);
        }
        return hbox(std::move(spans));
    }

} // namespace

bool syntax_type_supported(std::string_view type)
{
    return language_for_type(type) != nullptr;
}

std::string syntax_type_for_path(std::string_view path)
{
    const LanguageDefinition* language = language_for_path(path);
    return language == nullptr ? std::string { } : std::string(language->name);
}

Elements highlight_code(std::string_view code, std::string_view type)
{
    std::vector<SyntaxStyle> styles;
    if (const LanguageDefinition* language = language_for_type(type)) {
        styles = syntax_styles(code, *language);
    } else {
        styles.assign(code.size(), SyntaxStyle::PLAIN);
    }

    Elements lines;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t newline = code.find('\n', begin);
        const std::size_t end
            = newline == std::string_view::npos ? code.size() : newline;
        lines.push_back(render_line(code, styles, begin, end));
        if (newline == std::string_view::npos) {
            break;
        }
        begin = newline + 1;
    }
    return lines;
}

Element highlight_code_line(std::string_view code, std::string_view type)
{
    Elements lines = highlight_code(code, type);
    return lines.empty() ? text("") | color(PANEL_FG)
                         : std::move(lines.front());
}

} // namespace ursa
