#include "ui/ui.h"

extern "C" {
#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>
#include <table.h>
}

#include <ftxui/dom/flexbox_config.hpp>
#include <ftxui/dom/table.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <algorithm>

namespace imza {

using namespace ftxui;

namespace {

    struct Style {
        bool bold = false;
        bool emph = false;
        bool code = false;
        bool link = false;
        std::string url;
    };

    struct TableSpec {
        uint16_t cols = 0;
        std::vector<uint8_t> aligns; // 'l' | 'c' | 'r', per column
    };

    cmark_node* parse(std::string_view md)
    {
        static const bool extensions = [] {
            cmark_gfm_core_extensions_ensure_registered();
            return true;
        }();
        (void)extensions;
        cmark_parser* parser        = cmark_parser_new(CMARK_OPT_DEFAULT);
        cmark_syntax_extension* ext = cmark_find_syntax_extension("table");
        if (ext) {
            cmark_parser_attach_syntax_extension(parser, ext);
        }
        cmark_parser_feed(parser, md.data(), md.size());
        cmark_node* doc = cmark_parser_finish(parser);
        cmark_parser_free(parser);
        return doc;
    }

    class FtxuiSink;

    // Single cmark walk driving the sink. The sink receives block /
    // inline events; table structure is buffered by the walker and delivered as
    // begin / cell / row / end calls so sinks never touch cmark themselves.
    void walk_markdown(cmark_node* doc, FtxuiSink& s);

    class FtxuiSink {
    public:
        explicit FtxuiSink(int width)
            : _width(width)
        {
        }

        void code_block(std::string_view lit, const char* fence_info)
        {
            const std::string_view type = fence_info == nullptr
                ? std::string_view { }
                : std::string_view(fence_info);
            const int content_width     = std::max(20, _width - 6);
            Elements lines
                = highlighted_rows(lit, type, content_width, PANEL_FG_DIM);
            if (lines.empty()) {
                lines.push_back(ftxui::text(""));
            }
            _add(vbox(std::move(lines)) | bgcolor(PANEL_COLOR) | borderLight);
        }

        void text(std::string_view body, const Style& fl)
        {
            if (_quote_depth > 0 && _quote_first) {
                _quote_first = false;
                if (body.rfind("[!ERROR]", 0) == 0) {
                    body.remove_prefix(8);
                    _quote_alert = true;
                }
            }
            if (_in_cell) {
                _cell_buf += body;
                return;
            }
            if (_in_paragraph) {
                _push_words(body, fl);
            }
        }

        void softbreak()
        {
            if (_in_cell) {
                _cell_buf += ' ';
            } else if (_in_paragraph) {
                _needs_sep = true;
            }
        }

        // Flexbox wrapping cannot represent hard breaks mid-paragraph; treat
        // them as soft breaks.
        void linebreak() { softbreak(); }

        void paragraph_begin()
        {
            _in_paragraph = true;
            _words.clear();
            _needs_sep = false;
        }

        void paragraph_end()
        {
            _in_paragraph = false;
            _flush_words(_passthrough);
        }

        void heading_begin(int lvl)
        {
            _in_paragraph = true;
            _words.clear();
            _needs_sep     = false;
            _heading_level = lvl;
        }

        void heading_end()
        {
            const int lvl                    = _heading_level;
            _heading_level                   = 0;
            _in_paragraph                    = false;
            static const Color HEAD_COLORS[] = {
                PANEL_FG,
                HL_CYAN,
                HL_MAGENTA,
                HL_YELLOW,
                HL_GREEN,
                HL_BLUE,
            };
            Decorator decorate = [lvl](Element e) {
                const int idx = (lvl < 1 || lvl > 6) ? 0 : lvl - 1;
                return std::move(e) | bold | color(HEAD_COLORS[idx]);
            };
            _flush_words(std::move(decorate));
        }

        void quote_begin()
        {
            _frames.emplace_back();
            ++_quote_depth;
            _quote_first = true;
        }

        void quote_end()
        {
            Element body = _frames.back().empty() ? ftxui::text("")
                                                  : vbox(_frames.back());
            _frames.pop_back();
            if (_quote_alert) {
                body |= bold | color(HL_RED);
                _quote_alert = false;
            }
            --_quote_depth;
            _add(std::move(body) | bgcolor(PANEL_COLOR_FOCUS));
        }

        void list_begin(bool) { _lists.emplace_back(); }
        void list_end()
        {
            Elements list = std::move(_lists.back());
            _lists.pop_back();
            Element body
                = list.empty() ? ftxui::text("") : vbox(std::move(list));
            _add(std::move(body));
        }

        void item_begin(std::string_view prefix)
        {
            _frames.emplace_back();
            _item_prefixes.emplace_back(prefix);
        }

        void item_end()
        {
            Element body = _frames.back().empty() ? ftxui::text("")
                                                  : vbox(_frames.back());
            _frames.pop_back();
            Element label = ftxui::text(_item_prefixes.back());
            _item_prefixes.pop_back();
            Element item = hbox({ std::move(label), std::move(body) });
            if (_lists.empty()) {
                _add(std::move(item));
            } else {
                _lists.back().push_back(std::move(item));
            }
        }

        void thematic_break() { _add(separator()); }

        void table_begin(const TableSpec& spec)
        {
            _aligns = spec.aligns;
            _rows.clear();
            _header_flags.clear();
        }

        void row_begin() { _rows.emplace_back(); }
        void row_end(bool header) { _header_flags.push_back(header); }

        void cell_begin()
        {
            _cell_buf.clear();
            _in_cell = true;
        }

        void cell_end()
        {
            _rows.back().push_back(_cell_buf);
            _in_cell = false;
        }

        void table_end()
        {
            if (_rows.empty() || _rows[0].empty()) {
                _rows.clear();
                _header_flags.clear();
                _aligns.clear();
                return;
            }
            Table table(_rows);
            table.SelectAll().Border(LIGHT);
            table.SelectAll().SeparatorVertical(LIGHT);
            if (_header_flags[0]) {
                table.SelectRow(0).Decorate(bold);
            }
            for (size_t c = 0; c < _aligns.size(); ++c) {
                if (_aligns[c] == 'r') {
                    table.SelectColumn(static_cast<int>(c))
                        .DecorateCells(align_right);
                } else if (_aligns[c] == 'c') {
                    table.SelectColumn(static_cast<int>(c))
                        .DecorateCells(hcenter);
                }
            }
            _add(table.Render());
            _rows.clear();
            _header_flags.clear();
            _aligns.clear();
        }

        Element take()
        {
            return _root.empty() ? ftxui::text("") : vbox(std::move(_root));
        }

    private:
        using Decorator = std::function<Element(Element)>;

        void _add(Element e)
        {
            if (!_frames.empty()) {
                _frames.back().push_back(std::move(e));
            } else {
                _root.push_back(std::move(e));
            }
        }

        static Element _passthrough(Element e) { return e; }

        void _flush_words(const Decorator& extra)
        {
            if (_words.empty()) {
                return;
            }
            _add(extra(flexbox(std::move(_words))));
            _words.clear();
        }

        void _push_word(std::string_view word, const Style& fl)
        {
            if (_needs_sep && !_words.empty()) {
                _words.push_back(ftxui::text(" "));
            }
            _needs_sep = false;
            // Flexbox cannot break an over-long word; hard-split it so
            // paragraphs never overflow the pane width.
            const std::size_t budget = std::max<std::size_t>(
                8, static_cast<std::size_t>(std::max(8, _width - 2)));
            while (word.size() > budget) {
                _words.push_back(_styled(word.substr(0, budget), fl));
                _words.push_back(ftxui::text(" "));
                word.remove_prefix(budget);
            }
            _words.push_back(_styled(word, fl));
        }

        void _push_words(std::string_view body, const Style& fl)
        {
            size_t i = 0;
            while (i < body.size()) {
                const bool gap = body[i] == ' ';
                while (i < body.size() && body[i] == ' ') {
                    ++i;
                }
                if (gap) {
                    _needs_sep = true;
                }
                if (i == body.size()) {
                    break;
                }
                const size_t start = i;
                while (i < body.size() && body[i] != ' ') {
                    ++i;
                }
                _push_word(body.substr(start, i - start), fl);
            }
        }

        static Element _styled(std::string_view body, const Style& fl)
        {
            Element e = ftxui::text(std::string(body));
            if (fl.bold) {
                e |= bold;
            }
            if (fl.emph) {
                e |= italic;
            }
            if (fl.code) {
                e |= dim;
                e |= bgcolor(PANEL_COLOR_FOCUS);
            }
            if (fl.link) {
                e |= underlined;
                e |= color(HL_CYAN);
                if (!fl.url.empty()) {
                    e |= hyperlink(fl.url);
                }
            }
            return e;
        }

        std::vector<Elements> _frames;
        std::vector<std::string> _item_prefixes;
        std::vector<Elements> _lists;
        Elements _root;
        int _width;

        bool _in_paragraph = false;
        // Open quote state: [!ERROR] on the first literal tints the whole
        // quote red; nested frames keep the flag until the outermost close.
        int _quote_depth  = 0;
        bool _quote_first = false;
        bool _quote_alert = false;
        bool _in_cell     = false;
        bool _needs_sep   = false;
        Elements _words;
        int _heading_level = 0;

        std::string _cell_buf;
        std::vector<std::vector<std::string>> _rows;
        std::vector<bool> _header_flags;
        std::vector<uint8_t> _aligns;
    };

    void walk_markdown(cmark_node* doc, FtxuiSink& s)
    {
        cmark_iter* iter = cmark_iter_new(doc);

        int table_depth    = 0;
        bool row_is_header = false;
        TableSpec spec;

        std::vector<int> lists; // 1 = ordered, 0 = bullet; depth via size
        int item_index = 0;

        Style st;

        // Table node types are runtime values (cmark-gfm extension), so plain
        // comparisons instead of switch labels.
        auto is_block = [](cmark_node_type t) {
            return t == CMARK_NODE_PARAGRAPH || t == CMARK_NODE_HEADING
                || t == CMARK_NODE_LIST || t == CMARK_NODE_ITEM
                || t == CMARK_NODE_CODE_BLOCK || t == CMARK_NODE_BLOCK_QUOTE
                || t == CMARK_NODE_THEMATIC_BREAK || t == CMARK_NODE_HTML_BLOCK
                || t == CMARK_NODE_TABLE;
        };

        cmark_event_type ev;
        while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
            cmark_node* node        = cmark_iter_get_node(iter);
            const cmark_node_type t = cmark_node_get_type(node);
            const bool enter        = ev == CMARK_EVENT_ENTER;

            // Inside a table only the table structure and inline content flow
            // through; block-level nodes are dropped (cells hold inlines only).
            if (table_depth > 0 && t != CMARK_NODE_TABLE
                && t != CMARK_NODE_TABLE_ROW && t != CMARK_NODE_TABLE_CELL
                && is_block(t)) {
                continue;
            }

            if (t == CMARK_NODE_TABLE) {
                if (enter) {
                    spec.cols = cmark_gfm_extensions_get_table_columns(node);
                    spec.aligns.clear();
                    const uint8_t* al
                        = cmark_gfm_extensions_get_table_alignments(node);
                    for (uint16_t i = 0; al && i < spec.cols; ++i) {
                        spec.aligns.push_back(al[i]);
                    }
                    s.table_begin(spec);
                    table_depth = 1;
                } else {
                    s.table_end();
                    table_depth = 0;
                }
                continue;
            }
            if (t == CMARK_NODE_TABLE_ROW) {
                if (enter) {
                    row_is_header
                        = cmark_gfm_extensions_get_table_row_is_header(node)
                        != 0;
                    s.row_begin();
                } else {
                    s.row_end(row_is_header);
                }
                continue;
            }
            if (t == CMARK_NODE_TABLE_CELL) {
                if (enter) {
                    s.cell_begin();
                } else {
                    s.cell_end();
                }
                continue;
            }

            if (enter) {
                switch (t) {
                case CMARK_NODE_PARAGRAPH: s.paragraph_begin(); break;
                case CMARK_NODE_HEADING:
                    s.heading_begin(cmark_node_get_heading_level(node));
                    break;
                case CMARK_NODE_CODE_BLOCK:
                    s.code_block(cmark_node_get_literal(node),
                        cmark_node_get_fence_info(node));
                    break;
                case CMARK_NODE_BLOCK_QUOTE: s.quote_begin(); break;
                case CMARK_NODE_LIST:
                    lists.push_back(
                        cmark_node_get_list_type(node) == CMARK_ORDERED_LIST
                            ? 1
                            : 0);
                    // Restart numbering per list so a bullet list does
                    // not skew the next ordered list's first marker;
                    // start honors the source's first number.
                    item_index = cmark_node_get_list_start(node) - 1;
                    s.list_begin(lists.back() == 1);
                    break;
                case CMARK_NODE_ITEM: {
                    std::string prefix;
                    if (!lists.empty() && lists.back() == 1) {
                        ++item_index;
                        prefix = std::to_string(item_index) + ". ";
                    } else {
                        prefix = "- ";
                    }
                    s.item_begin(prefix);
                    break;
                }
                case CMARK_NODE_THEMATIC_BREAK: s.thematic_break(); break;
                case CMARK_NODE_TEXT:
                    s.text(cmark_node_get_literal(node), st);
                    break;
                case CMARK_NODE_CODE:
                    st.code = true;
                    s.text(cmark_node_get_literal(node), st);
                    st.code = false;
                    break;
                case CMARK_NODE_SOFTBREAK: s.softbreak(); break;
                case CMARK_NODE_LINEBREAK: s.linebreak(); break;
                case CMARK_NODE_STRONG: st.bold = true; break;
                case CMARK_NODE_EMPH: st.emph = true; break;
                case CMARK_NODE_LINK:
                    st.link = true;
                    st.url  = cmark_node_get_url(node);
                    break;
                default: // HTML_BLOCK / HTML_INLINE / IMAGE / ...
                    break;
                }
            } else {
                switch (t) {
                case CMARK_NODE_PARAGRAPH: s.paragraph_end(); break;
                case CMARK_NODE_HEADING: s.heading_end(); break;
                case CMARK_NODE_BLOCK_QUOTE: s.quote_end(); break;
                case CMARK_NODE_LIST:
                    s.list_end();
                    if (!lists.empty()) {
                        lists.pop_back();
                    }
                    break;
                case CMARK_NODE_ITEM: s.item_end(); break;
                case CMARK_NODE_STRONG: st.bold = false; break;
                case CMARK_NODE_EMPH: st.emph = false; break;
                case CMARK_NODE_LINK:
                    st.link = false;
                    st.url.clear();
                    break;
                default: break;
                }
            }
        }

        cmark_iter_free(iter);
    }

} // namespace

Element render_markdown_element(std::string_view md, int width)
{
    FtxuiSink sink(width);
    cmark_node* doc = parse(md);
    if (doc) {
        walk_markdown(doc, sink);
    }
    cmark_node_free(doc);
    return sink.take();
}

namespace {

    // Byte offsets of every line start; the sentinel past the last line
    // makes end-of-text slices uniform.
    std::vector<std::size_t> line_offsets(std::string_view md)
    {
        std::vector<std::size_t> offsets { 0 };
        for (std::size_t i = 0; i < md.size(); ++i) {
            if (md[i] == '\n') {
                offsets.push_back(i + 1);
            }
        }
        offsets.push_back(md.size());
        return offsets;
    }

    std::string_view line_slice(std::string_view md,
        const std::vector<std::size_t>& offsets, int start_line, int end_line)
    {
        const std::size_t last  = offsets.size() - 2;
        const std::size_t begin = offsets[std::clamp<std::size_t>(
            static_cast<std::size_t>(std::max(1, start_line) - 1), 0, last)];
        const std::size_t end   = offsets[std::clamp<std::size_t>(
            static_cast<std::size_t>(std::max(1, end_line)), 0, last + 1)];
        return begin <= end ? md.substr(begin, end - begin)
                            : std::string_view { };
    }

    std::string_view first_line(std::string_view text)
    {
        while (!text.empty()
            && (text.back() == '\n' || text.back() == '\r'
                || text.back() == ' ')) {
            text.remove_suffix(1);
        }
        return text;
    }

} // namespace

std::vector<MarkdownBlock> render_markdown_blocks(
    std::string_view md, int width)
{
    std::vector<MarkdownBlock> blocks;
    cmark_node* doc = parse(md);
    if (doc == nullptr) {
        return blocks;
    }
    const std::vector<std::size_t> offsets = line_offsets(md);
    // Top-level blocks; a list expands to one selectable block per item so
    // a single "1." / "-" line can carry its own annotation. Rendering each
    // slice separately keeps tables, code, and quotes intact, and the
    // excerpt is the block's first source line - the natural
    // imza.plan.edit old-string target.
    const std::function<void(cmark_node*)> emit = [&](cmark_node* node) {
        MarkdownBlock block;
        block.line    = cmark_node_get_start_line(node);
        block.source  = std::string(first_line(line_slice(md, offsets,
            cmark_node_get_start_line(node), cmark_node_get_start_line(node))));
        block.element = render_markdown_element(
            line_slice(md, offsets, cmark_node_get_start_line(node),
                cmark_node_get_end_line(node)),
            width);
        blocks.push_back(std::move(block));
    };
    for (cmark_node* node = cmark_node_first_child(doc); node != nullptr;
        node              = cmark_node_next(node)) {
        if (cmark_node_get_type(node) == CMARK_NODE_LIST) {
            for (cmark_node* item     = cmark_node_first_child(node);
                item != nullptr; item = cmark_node_next(item)) {
                emit(item);
            }
            continue;
        }
        emit(node);
    }
    cmark_node_free(doc);
    return blocks;
}

} // namespace imza
