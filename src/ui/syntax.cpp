#include "ui/ui.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/language_match.h"
#include "common/ts_query.h"
#include "common/util.h"

namespace imza {

using namespace ftxui;

namespace {

    enum class SyntaxStyle : std::uint8_t {
        PLAIN,
        KEYWORD,
        TYPE,
        STRING,
        NUMBER,
        COMMENT,
        SPECIAL,
    };

    using LanguageFunction = const TSLanguage* (*)();

    struct LanguageSpec {
        std::string_view name;
        std::span<const std::string_view> extensions;
        std::span<const std::string_view> filenames;
        LanguageFunction load_language { };
        std::string_view highlight_query;
    };

#include "syntax_registry.inc"

    struct LanguageDefinition {
        std::string_view name;
        std::span<const std::string_view> extensions;
        std::span<const std::string_view> filenames;
        LanguageFunction load_language { };
        std::string_view highlight_query;
        struct Runtime {
            std::once_flag initialized;
            const TSLanguage* language { };
            TsQueryPtr query;
            TsParserPtr parser;
            TsQueryCursorPtr cursor;
            TsPredicateSet predicates;
            std::mutex use_mutex;
        };
        std::unique_ptr<Runtime> runtime;
    };

    TsQueryPtr make_query(const TSLanguage* language, std::string_view source)
    {
        uint32_t offset    = 0;
        TSQueryError error = TSQueryErrorNone;
        return TsQueryPtr(ts_query_new(language, source.data(),
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
        std::string type      = to_lower(hint.substr(0, end));
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
        return match_language_entry<LanguageDefinition>(languages(), path);
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
        case SyntaxStyle::KEYWORD: return HL_GREEN;
        case SyntaxStyle::TYPE: return HL_CYAN;
        case SyntaxStyle::STRING: return HL_YELLOW;
        case SyntaxStyle::NUMBER: return HL_MAGENTA;
        case SyntaxStyle::COMMENT: return PANEL_FG_DIM;
        case SyntaxStyle::SPECIAL: return HL_BLUE;
        case SyntaxStyle::PLAIN: return PANEL_FG;
        }
        return PANEL_FG;
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
                runtime.predicates = ts_compile_predicates(runtime.query.get());
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
        TsTreePtr tree(ts_parser_parse_string(runtime.parser.get(), nullptr,
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
            if (!ts_predicates_hold(runtime.predicates, query, match, code)) {
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

std::vector<std::vector<Element>> highlight_code_wrapped(
    std::string_view code, std::string_view type, int width)
{
    std::vector<SyntaxStyle> styles;
    if (const LanguageDefinition* language = language_for_type(type)) {
        styles = syntax_styles(code, *language);
    } else {
        styles.assign(code.size(), SyntaxStyle::PLAIN);
    }

    std::vector<std::vector<Element>> lines;
    for_each_line(code, [&](std::size_t begin, std::string_view line) {
        std::vector<Element> rows;
        for (const auto& [row_begin, row_end] : wrap_row_ranges(line, width)) {
            rows.push_back(
                render_line(code, styles, begin + row_begin, begin + row_end));
        }
        lines.push_back(std::move(rows));
    });
    return lines;
}

} // namespace imza
