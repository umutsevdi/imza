#include "ui/ui.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    extern "C" {
    const TSLanguage* tree_sitter_cpp();
    const TSLanguage* tree_sitter_html();
    const TSLanguage* tree_sitter_css();
    const TSLanguage* tree_sitter_javascript();
    const TSLanguage* tree_sitter_typescript();
    const TSLanguage* tree_sitter_go();
    const TSLanguage* tree_sitter_rust();
    const TSLanguage* tree_sitter_swift();
    const TSLanguage* tree_sitter_dockerfile();
    const TSLanguage* tree_sitter_java();
    const TSLanguage* tree_sitter_cmake();
    const TSLanguage* tree_sitter_dart();
    const TSLanguage* tree_sitter_make();
    const TSLanguage* tree_sitter_json();
    const TSLanguage* tree_sitter_lua();
    const TSLanguage* tree_sitter_python();
    const TSLanguage* tree_sitter_php();
    const TSLanguage* tree_sitter_bash();
    const TSLanguage* tree_sitter_powershell();
    }

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
        std::string_view name;
        std::vector<std::string_view> aliases;
        std::vector<std::string_view> extensions;
        std::vector<std::string_view> filenames;
        const TSLanguage* language { };
        QueryPtr query;
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

    using LanguageFunction = const TSLanguage* (*)();

    void add_language(std::vector<LanguageDefinition>& languages,
        std::string_view name, LanguageFunction language,
        std::string_view query, std::initializer_list<std::string_view> aliases,
        std::initializer_list<std::string_view> extensions,
        std::initializer_list<std::string_view> filenames = { })
    {
        const TSLanguage* ts_language = language();
        languages.push_back(LanguageDefinition { name, aliases, extensions,
            filenames, ts_language, make_query(ts_language, query) });
    }

    std::vector<LanguageDefinition> make_languages()
    {
        std::vector<LanguageDefinition> languages;
        languages.reserve(19);
        add_language(languages, "cpp", tree_sitter_cpp, CPP_HIGHLIGHTS,
            { "cpp", "c++", "c" },
            { "cpp", "cc", "cxx", "c", "hpp", "hh", "hxx", "h" });
        add_language(languages, "html", tree_sitter_html, HTML_HIGHLIGHTS,
            { "html", "htm" }, { "html", "htm" });
        add_language(languages, "css", tree_sitter_css, CSS_HIGHLIGHTS,
            { "css" }, { "css" });
        add_language(languages, "javascript", tree_sitter_javascript,
            JAVASCRIPT_HIGHLIGHTS, { "javascript", "js", "jsx" },
            { "js", "mjs", "cjs", "jsx" });
        add_language(languages, "typescript", tree_sitter_typescript,
            TYPESCRIPT_HIGHLIGHTS, { "typescript", "ts" },
            { "ts", "mts", "cts" });
        add_language(languages, "go", tree_sitter_go, GO_HIGHLIGHTS,
            { "go", "golang" }, { "go" });
        add_language(languages, "rust", tree_sitter_rust, RUST_HIGHLIGHTS,
            { "rust", "rs" }, { "rs" });
        add_language(languages, "swift", tree_sitter_swift, SWIFT_HIGHLIGHTS,
            { "swift" }, { "swift" });
        add_language(languages, "dockerfile", tree_sitter_dockerfile,
            DOCKERFILE_HIGHLIGHTS, { "dockerfile", "docker", "containerfile" },
            { }, { "dockerfile", "containerfile" });
        add_language(languages, "java", tree_sitter_java, JAVA_HIGHLIGHTS,
            { "java" }, { "java" });
        add_language(languages, "cmake", tree_sitter_cmake, CMAKE_HIGHLIGHTS,
            { "cmake" }, { "cmake" }, { "cmakelists.txt" });
        add_language(languages, "dart", tree_sitter_dart, DART_HIGHLIGHTS,
            { "dart" }, { "dart" });
        add_language(languages, "make", tree_sitter_make, MAKE_HIGHLIGHTS,
            { "make", "makefile" }, { "mk", "mak" },
            { "makefile", "gnumakefile", "bsdmakefile" });
        add_language(languages, "json", tree_sitter_json, JSON_HIGHLIGHTS,
            { "json" }, { "json" });
        add_language(languages, "lua", tree_sitter_lua, LUA_HIGHLIGHTS,
            { "lua" }, { "lua" });
        add_language(languages, "python", tree_sitter_python, PYTHON_HIGHLIGHTS,
            { "python", "py" }, { "py", "pyw", "pyi" });
        add_language(languages, "php", tree_sitter_php, PHP_HIGHLIGHTS,
            { "php" }, { "php", "phtml" });
        add_language(languages, "bash", tree_sitter_bash, BASH_HIGHLIGHTS,
            { "bash", "sh", "shell" }, { "sh", "bash" },
            { ".bashrc", ".bash_profile", ".profile" });
        add_language(languages, "powershell", tree_sitter_powershell,
            POWERSHELL_HIGHLIGHTS, { "powershell", "pwsh", "ps1" },
            { "ps1", "psm1", "psd1" });
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
                      || contains(candidate.aliases, type)
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
        const std::string filename = lower(file.filename().string());
        const auto& registry       = languages();
        const auto language        = std::ranges::find_if(
            registry, [&extension, &filename](const auto& candidate) {
                return contains(candidate.extensions, extension)
                    || contains(candidate.filenames, filename);
            });
        return language == registry.end() ? nullptr : &*language;
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
            std::optional<uint32_t> capture_id;
            std::vector<std::string_view> values;
            while (
                at < count && steps[at].type != TSQueryPredicateStepTypeDone) {
                if (steps[at].type == TSQueryPredicateStepTypeCapture
                    && !capture_id.has_value()) {
                    capture_id = steps[at].value_id;
                } else if (steps[at].type == TSQueryPredicateStepTypeString) {
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
            const std::string_view body
                = capture_text(match, *capture_id, code);

            bool matched = true;
            if (operation == "eq?" || operation == "not-eq?") {
                matched = body == values.front();
            } else if (operation == "any-of?" || operation == "not-any-of?") {
                matched = std::ranges::find(values, body) != values.end();
            } else if (operation == "match?" || operation == "not-match?"
                || operation == "lua-match?") {
                try {
                    const std::string pattern = operation == "lua-match?"
                        ? lua_regex(values.front())
                        : std::string(values.front());
                    matched                   = std::regex_search(
                        body.begin(), body.end(), std::regex(pattern));
                } catch (const std::regex_error&) {
                    return false;
                }
            }
            if (operation == "not-eq?" || operation == "not-match?"
                || operation == "not-any-of?") {
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
        const TSQuery* query = language.query.get();
        if (query == nullptr) {
            return styles;
        }
        ParserPtr parser(ts_parser_new());
        if (!parser
            || !ts_parser_set_language(parser.get(), language.language)) {
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
