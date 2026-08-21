#include "render.hpp"

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>
#include <table.h>

#include <ftxui/dom/flexbox_config.hpp>
#include <ftxui/dom/table.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ursa {

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

    cmark_node* parse(const std::string& md)
    {
        cmark_gfm_core_extensions_ensure_registered();
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

    // Single cmark walk driving a compile-time sink. The sink receives block /
    // inline events; table structure is buffered by the walker and delivered as
    // begin / cell / row / end calls so sinks never touch cmark themselves.
    template <typename Sink> void walk_markdown(cmark_node* doc, Sink& s)
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
                case CMARK_NODE_PARAGRAPH:
                    s.paragraph_begin(lists.empty());
                    break;
                case CMARK_NODE_HEADING:
                    s.heading_begin(cmark_node_get_heading_level(node));
                    break;
                case CMARK_NODE_CODE_BLOCK:
                    s.code_block(cmark_node_get_literal(node));
                    break;
                case CMARK_NODE_BLOCK_QUOTE: s.quote_begin(); break;
                case CMARK_NODE_LIST:
                    lists.push_back(
                        cmark_node_get_list_type(node) == CMARK_ORDERED_LIST
                            ? 1
                            : 0);
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

    class FtxuiSink {
    public:
        void text(std::string_view body, const Style& fl)
        {
            if (in_cell_) {
                cell_buf_ += body;
                return;
            }
            if (in_paragraph_) {
                push_words(body, fl);
            }
        }

        void softbreak()
        {
            if (in_cell_) {
                cell_buf_ += ' ';
            } else if (in_paragraph_) {
                words_.push_back(ftxui::text(" "));
            }
        }

        // Flexbox wrapping cannot represent hard breaks mid-paragraph; treat
        // them as soft breaks.
        void linebreak() { softbreak(); }

        void paragraph_begin(bool)
        {
            in_paragraph_ = true;
            words_.clear();
        }

        void paragraph_end()
        {
            in_paragraph_ = false;
            flush_words(passthrough);
        }

        void heading_begin(int lvl)
        {
            in_paragraph_ = true;
            words_.clear();
            heading_level_ = lvl;
        }

        void heading_end()
        {
            const int lvl                    = heading_level_;
            heading_level_                   = 0;
            in_paragraph_                    = false;
            static const Color HEAD_COLORS[] = {
                Color::White,
                Color::CyanLight,
                Color::MagentaLight,
                Color::YellowLight,
                Color::GreenLight,
                Color::BlueLight,
            };
            Decorator decorate = [lvl](Element e) {
                const int idx = (lvl < 1 || lvl > 6) ? 0 : lvl - 1;
                return std::move(e) | bold | color(HEAD_COLORS[idx]);
            };
            flush_words(std::move(decorate));
        }

        void code_block(std::string_view lit)
        {
            Elements lines;
            size_t start = 0;
            while (start <= lit.size()) {
                size_t end = lit.find('\n', start);
                if (end == std::string_view::npos) {
                    end = lit.size();
                }
                lines.push_back(
                    ftxui::text(std::string(lit.substr(start, end - start)))
                    | color(Color::Palette256(245))
                    | bgcolor(Color::Palette256(234)));
                if (end == lit.size()) {
                    break;
                }
                start = end + 1;
            }
            if (lines.empty()) {
                lines.push_back(ftxui::text(""));
            }
            add(vbox(std::move(lines)) | bgcolor(Color::Palette256(234))
                | borderLight);
        }

        void quote_begin() { frames_.emplace_back(); }

        void quote_end()
        {
            Element body = frames_.back().empty() ? ftxui::text("")
                                                  : vbox(frames_.back());
            frames_.pop_back();
            add(std::move(body) | bgcolor(Color::Palette256(237)));
        }

        void list_begin(bool) { lists_.push_back(ListFrame { }); }
        void list_end()
        {
            ListFrame list = std::move(lists_.back());
            lists_.pop_back();
            Element body = list.items.empty() ? ftxui::text("")
                                              : vbox(std::move(list.items));
            add(std::move(body));
        }

        void item_begin(std::string_view prefix)
        {
            frames_.emplace_back();
            item_prefixes_.push_back(std::string(prefix));
        }

        void item_end()
        {
            Element body = frames_.back().empty() ? ftxui::text("")
                                                  : vbox(frames_.back());
            frames_.pop_back();
            Element label = ftxui::text(item_prefixes_.back());
            item_prefixes_.pop_back();
            Element item = hbox({ std::move(label), std::move(body) });
            if (lists_.empty()) {
                add(std::move(item));
            } else {
                lists_.back().items.push_back(std::move(item));
            }
        }

        void thematic_break() { add(separator()); }

        void table_begin(const TableSpec& spec)
        {
            aligns_ = spec.aligns;
            rows_.clear();
            header_flags_.clear();
        }

        void row_begin() { rows_.emplace_back(); }
        void row_end(bool header) { header_flags_.push_back(header); }

        void cell_begin()
        {
            cell_buf_.clear();
            in_cell_ = true;
        }

        void cell_end()
        {
            rows_.back().push_back(cell_buf_);
            in_cell_ = false;
        }

        void table_end()
        {
            if (rows_.empty() || rows_[0].empty()) {
                rows_.clear();
                header_flags_.clear();
                aligns_.clear();
                return;
            }
            Table table(rows_);
            table.SelectAll().Border(LIGHT);
            table.SelectAll().SeparatorVertical(LIGHT);
            if (header_flags_[0]) {
                table.SelectRow(0).Decorate(bold);
            }
            for (size_t c = 0; c < aligns_.size(); ++c) {
                if (aligns_[c] == 'r') {
                    table.SelectColumn(static_cast<int>(c))
                        .DecorateCells(align_right);
                } else if (aligns_[c] == 'c') {
                    table.SelectColumn(static_cast<int>(c))
                        .DecorateCells(hcenter);
                }
            }
            add(table.Render());
            rows_.clear();
            header_flags_.clear();
            aligns_.clear();
        }

        Element take()
        {
            return root_.empty() ? ftxui::text("") : vbox(std::move(root_));
        }

    private:
        using Decorator = std::function<Element(Element)>;

        struct ListFrame {
            std::vector<Element> items;
        };

        void add(Element e)
        {
            if (!frames_.empty()) {
                frames_.back().push_back(std::move(e));
            } else {
                root_.push_back(std::move(e));
            }
        }

        static Element passthrough(Element e) { return e; }

        void flush_words(Decorator extra)
        {
            if (words_.empty()) {
                return;
            }
            add(extra(flexbox(std::move(words_))));
            words_.clear();
        }

        void push_words(std::string_view body, const Style& fl)
        {
            size_t i = 0;
            while (i < body.size()) {
                while (i < body.size() && body[i] == ' ') {
                    ++i;
                }
                const size_t start = i;
                while (i < body.size() && body[i] != ' ') {
                    ++i;
                }
                if (i == start) {
                    continue;
                }
                words_.push_back(styled(body.substr(start, i - start), fl));
                if (i < body.size()) {
                    words_.push_back(ftxui::text(" "));
                }
            }
        }

        static Element styled(std::string_view body, const Style& fl)
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
                e |= bgcolor(Color::Palette256(236));
            }
            if (fl.link) {
                e |= underlined;
                e |= color(Color::CyanLight);
                if (!fl.url.empty()) {
                    e |= hyperlink(fl.url);
                }
            }
            return e;
        }

        std::vector<Elements> frames_;
        std::vector<std::string> item_prefixes_;
        std::vector<ListFrame> lists_;
        Elements root_;

        bool in_paragraph_ = false;
        bool in_cell_      = false;
        Elements words_;
        int heading_level_ = 0;

        std::string cell_buf_;
        std::vector<std::vector<std::string>> rows_;
        std::vector<bool> header_flags_;
        std::vector<uint8_t> aligns_;
    };

} // namespace

Element render_markdown_element(std::string_view md)
{
    FtxuiSink sink;
    cmark_node* doc = parse(std::string(md));
    if (doc) {
        walk_markdown(doc, sink);
    }
    cmark_node_free(doc);
    return sink.take();
}

} // namespace ursa
