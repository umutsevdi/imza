#include "ui/ui.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    extern "C" const TSLanguage* tree_sitter_cpp();

#include "syntax_queries.inc"

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

    struct LanguageDefinition {
        constexpr LanguageDefinition(std::string_view name_value,
            std::span<const std::string_view> alias_values,
            std::span<const std::string_view> extension_values,
            std::span<const std::string_view> filename_values,
            const TSLanguage* (&language_value)(), TSQuery* (&query_value)())
            : name(name_value)
            , aliases(alias_values)
            , extensions(extension_values)
            , filenames(filename_values)
            , language(language_value)
            , query(query_value)
        {
        }

        std::string_view name;
        std::span<const std::string_view> aliases;
        std::span<const std::string_view> extensions;
        std::span<const std::string_view> filenames;
        const TSLanguage* (&language)();
        TSQuery* (&query)();
    };

    std::string lower(std::string_view value)
    {
        std::string out(value);
        std::ranges::transform(out, out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    QueryPtr make_cpp_query()
    {
        uint32_t offset      = 0;
        TSQueryError error   = TSQueryErrorNone;
        const TSLanguage* ts = tree_sitter_cpp();
        return QueryPtr(ts_query_new(ts, CPP_HIGHLIGHTS.data(),
            static_cast<uint32_t>(CPP_HIGHLIGHTS.size()), &offset, &error));
    }

    TSQuery* cpp_query()
    {
        static QueryPtr query = make_cpp_query();
        return query.get();
    }

    constexpr std::array<std::string_view, 2> CPP_ALIASES { "cpp", "c++" };
    constexpr std::array<std::string_view, 7> CPP_EXTENSIONS { "cpp", "cc",
        "cxx", "hpp", "hh", "hxx", "h" };
    constexpr std::array<std::string_view, 0> CPP_FILENAMES { };
    constexpr std::array<LanguageDefinition, 1> LANGUAGES { LanguageDefinition {
        "cpp", CPP_ALIASES, CPP_EXTENSIONS, CPP_FILENAMES, tree_sitter_cpp,
        cpp_query } };

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
        const auto language
            = std::ranges::find_if(LANGUAGES, [&type](const auto& candidate) {
                  return type == candidate.name
                      || contains(candidate.aliases, type)
                      || contains(candidate.extensions, type);
              });
        return language == LANGUAGES.end() ? nullptr : &*language;
    }

    const LanguageDefinition* language_for_path(std::string_view path)
    {
        const std::filesystem::path file(path);
        std::string extension = lower(file.extension().string());
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(0, 1);
        }
        const std::string filename = lower(file.filename().string());
        const auto language        = std::ranges::find_if(
            LANGUAGES, [&extension, &filename](const auto& candidate) {
                return contains(candidate.extensions, extension)
                    || contains(candidate.filenames, filename);
            });
        return language == LANGUAGES.end() ? nullptr : &*language;
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
        if (capture.starts_with("type") || capture == "variable.builtin") {
            return SyntaxStyle::TYPE;
        }
        if (capture.starts_with("keyword") || capture.starts_with("operator")) {
            return SyntaxStyle::KEYWORD;
        }
        if (capture.starts_with("preproc") || capture.starts_with("function")
            || capture.starts_with("constructor") || capture == "module") {
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

    bool predicates_match(
        const TSQuery* query, const TSQueryMatch& match, std::string_view code)
    {
        uint32_t count                    = 0;
        const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(
            query, match.pattern_index, &count);
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
            if (at + 1 >= count
                || steps[at].type != TSQueryPredicateStepTypeCapture
                || steps[at + 1].type != TSQueryPredicateStepTypeString) {
                while (at < count
                    && steps[at].type != TSQueryPredicateStepTypeDone) {
                    ++at;
                }
                ++at;
                continue;
            }
            const uint32_t capture_id = steps[at++].value_id;
            uint32_t value_size       = 0;
            const char* value_raw     = ts_query_string_value_for_id(
                query, steps[at++].value_id, &value_size);
            const std::string_view value(value_raw, value_size);
            const std::string_view body = capture_text(match, capture_id, code);

            bool matched = true;
            if (operation == "eq?" || operation == "not-eq?") {
                matched = body == value;
            } else if (operation == "match?" || operation == "not-match?") {
                try {
                    matched = std::regex_search(body.begin(), body.end(),
                        std::regex(std::string(value)));
                } catch (const std::regex_error&) {
                    return false;
                }
            }
            if (operation == "not-eq?" || operation == "not-match?") {
                matched = !matched;
            }
            if (!matched) {
                return false;
            }
            while (
                at < count && steps[at].type != TSQueryPredicateStepTypeDone) {
                ++at;
            }
            ++at;
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
        const TSQuery* query = language.query();
        if (query == nullptr) {
            return styles;
        }
        ParserPtr parser(ts_parser_new());
        if (!parser
            || !ts_parser_set_language(parser.get(), language.language())) {
            return styles;
        }
        TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, code.data(),
            static_cast<uint32_t>(code.size())));
        CursorPtr cursor(ts_query_cursor_new());
        if (!tree || !cursor) {
            return styles;
        }
        ts_query_cursor_exec(
            cursor.get(), query, ts_tree_root_node(tree.get()));

        std::vector<uint32_t> span_sizes(
            code.size(), std::numeric_limits<uint32_t>::max());
        TSQueryMatch match { };
        uint32_t capture_index = 0;
        while (ts_query_cursor_next_capture(
            cursor.get(), &match, &capture_index)) {
            if (!predicates_match(query, match, code)) {
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
    std::vector<SyntaxStyle> styles(code.size(), SyntaxStyle::PLAIN);
    if (const LanguageDefinition* language = language_for_type(type)) {
        styles = syntax_styles(code, *language);
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
