#include "app/application_state.h"
#include "app/flows.h"
#include "common/modal.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "conversation/session.h"
#include "network/chat.h"
#include "ui/ui.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    constexpr int COL_PROVIDER = 12;
    constexpr int COL_MODEL    = 22;

    std::string fit(std::string value, int width)
    {
        if (static_cast<int>(value.size()) <= width) {
            return value;
        }
        return value.substr(0, std::max(1, width - 1)) + "…";
    }

    std::string money_text(double cost)
    {
        std::ostringstream o;
        o << '$' << std::fixed << std::setprecision(cost >= 1.0 ? 2 : 3)
          << cost;
        return o.str();
    }

    Element usage_table(const std::vector<std::vector<std::string>>& cells,
        const std::vector<int>& right_columns)
    {
        Table table(cells);
        table.SelectAll().Border(LIGHT);
        table.SelectAll().SeparatorVertical(LIGHT);
        table.SelectRow(0).Decorate(bold);
        for (const int column : right_columns) {
            table.SelectColumn(column).DecorateCells(align_right);
        }
        return table.Render();
    }

    CanvasView day_chart(const std::vector<UsageDayRow>& by_day)
    {
        CanvasView view;
        view.kind  = CanvasView::Kind::BAR;
        view.title = "Usage by day";
        for (const auto& day : by_day) {
            view.series.push_back(
                { day.day.size() >= 10 ? day.day.substr(5) : day.day,
                    { double(day.total) } });
        }
        return view;
    }

    Element model_table(const std::vector<UsageBreakdownRow>& by_model,
        std::uint64_t grand, bool with_total, double total_cost)
    {
        std::vector<std::vector<std::string>> rows = {
            { "PROVIDER", "MODEL", "TOKENS", "COST" },
        };
        for (const auto& row : by_model) {
            rows.push_back(
                { fit(row.provider, COL_PROVIDER), fit(row.model, COL_MODEL),
                    compact_number(row.total), money_text(row.cost) });
        }
        if (with_total) {
            rows.push_back(
                { "TOTAL", "", compact_number(grand), money_text(total_cost) });
        }
        return usage_table(rows, { 2, 3 });
    }

    Element provider_table(const std::vector<UsageBreakdownRow>& by_provider,
        std::uint64_t grand, bool with_total, double total_cost)
    {
        std::vector<std::vector<std::string>> rows = {
            { "PROVIDER", "TOKENS", "SHARE", "COST" },
        };
        for (const auto& row : by_provider) {
            const std::string share = grand > 0
                ? std::to_string(row.total * 100 / grand) + "%"
                : "-";
            rows.push_back({ fit(row.provider, COL_PROVIDER),
                compact_number(row.total), share, money_text(row.cost) });
        }
        if (with_total) {
            rows.push_back(
                { "TOTAL", compact_number(grand), "", money_text(total_cost) });
        }
        return usage_table(rows, { 1, 2, 3 });
    }

    std::vector<UsageBreakdownRow> rollup_by_provider(
        const std::vector<UsageBreakdownRow>& by_model)
    {
        std::map<std::string, UsageBreakdownRow> rows;
        for (const auto& row : by_model) {
            auto& agg    = rows[row.provider];
            agg.provider = row.provider;
            agg.prompt += row.prompt;
            agg.completion += row.completion;
            agg.total += row.total;
            agg.cost += row.cost;
        }
        std::vector<UsageBreakdownRow> out;
        out.reserve(rows.size());
        for (auto& [name, row] : rows) {
            out.push_back(std::move(row));
        }
        std::sort(out.begin(), out.end(),
            [](const UsageBreakdownRow& a, const UsageBreakdownRow& b) {
                return a.cost > b.cost;
            });
        return out;
    }

    // The most recent 30 days, oldest first.
    std::vector<UsageDayRow> recent_days(const std::vector<UsageDayRow>& by_day)
    {
        constexpr std::size_t MAX_DAYS = 30;
        if (by_day.size() <= MAX_DAYS) {
            return by_day;
        }
        return { by_day.end() - static_cast<std::ptrdiff_t>(MAX_DAYS),
            by_day.end() };
    }

    Element day_table(const std::vector<UsageDayRow>& days, std::uint64_t grand,
        bool with_total, double total_cost)
    {
        std::vector<std::vector<std::string>> rows = {
            { "DAY", "TOKENS", "SHARE", "COST" },
        };
        std::uint64_t shown = 0;
        for (const auto& day : days) {
            const std::string share = grand > 0
                ? std::to_string(day.total * 100 / grand) + "%"
                : "-";
            rows.push_back({ day.day, compact_number(day.total), share,
                money_text(day.cost) });
            shown += day.total;
        }
        if (with_total) {
            rows.push_back(
                { "TOTAL", compact_number(shown), "", money_text(total_cost) });
        }
        return usage_table(rows, { 1, 2, 3 });
    }

    Element spend_summary(const std::string& scope, std::uint64_t tokens,
        double cost, std::size_t day_count)
    {
        std::ostringstream body;
        body << scope << ": " << compact_number(tokens) << " tokens, "
             << money_text(cost) << " across " << day_count
             << (day_count == 1 ? " day." : " days.");
        return paragraph(body.str()) | dim;
    }

    Element render_session_tab(Session& session, int width)
    {
        Elements out;
        const std::vector<UsageEntry> log = session.usage_log();
        if (log.empty()) {
            out.push_back(text("No usage recorded yet") | dim);
            return vbox(std::move(out));
        }
        const GlobalUsageSummary summary
            = summarize_usage(log, format_local_time("%Y-%m-%d"));
        std::vector<UsageBreakdownRow> by_model = summary.by_model;
        std::sort(by_model.begin(), by_model.end(),
            [](const UsageBreakdownRow& a, const UsageBreakdownRow& b) {
                return a.cost > b.cost;
            });

        out.push_back(canvas_chart(day_chart(summary.by_day), width));
        out.push_back(separatorEmpty());
        out.push_back(text("By model") | bold);
        out.push_back(separatorEmpty());
        out.push_back(model_table(by_model, summary.total, false, 0.0));
        out.push_back(separatorEmpty());
        out.push_back(spend_summary("This session", summary.total,
            summary.total_cost, summary.by_day.size()));
        return vbox(std::move(out));
    }

    Element render_total_tab(const GlobalUsageSummary& summary, int width)
    {
        Elements out;
        if (summary.by_model.empty() && summary.by_day.empty()) {
            out.push_back(text("No saved usage yet") | dim);
            return vbox(std::move(out));
        }
        const std::vector<UsageDayRow> days = recent_days(summary.by_day);
        GlobalUsageSummary window;
        std::map<std::pair<std::string, std::string>, UsageBreakdownRow> rollup;
        for (const UsageDayRow& day : days) {
            window.total += day.total;
            window.total_cost += day.cost;
            for (const UsageModelRow& model : day.by_model) {
                auto& row    = rollup[{ model.provider, model.model }];
                row.provider = model.provider;
                row.model    = model.model;
                row.prompt += model.prompt;
                row.completion += model.completion;
                row.total += model.total;
                row.cost += model.cost;
            }
        }
        window.by_day = days;
        std::vector<UsageBreakdownRow> by_model;
        by_model.reserve(rollup.size());
        for (auto& [key, row] : rollup) {
            by_model.push_back(std::move(row));
        }
        std::sort(by_model.begin(), by_model.end(),
            [](const UsageBreakdownRow& a, const UsageBreakdownRow& b) {
                return a.cost > b.cost;
            });
        out.push_back(canvas_chart(day_chart(days), width));
        out.push_back(separatorEmpty());
        out.push_back(day_table(days, window.total, true, window.total_cost));
        out.push_back(separatorEmpty());
        out.push_back(text("By model") | bold);
        out.push_back(separatorEmpty());
        out.push_back(
            model_table(by_model, window.total, true, window.total_cost));
        out.push_back(separatorEmpty());
        out.push_back(text("By provider") | bold);
        out.push_back(separatorEmpty());
        out.push_back(provider_table(rollup_by_provider(by_model), window.total,
            true, window.total_cost));
        out.push_back(separatorEmpty());
        out.push_back(spend_summary("Last 30 days", window.total,
            window.total_cost, window.by_day.size()));
        return vbox(std::move(out));
    }

    class UsageView : public ComponentBase {
    public:
        explicit UsageView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
            , _session(_state->session)
        {
            _tab_names = { "Session", "Total" };
            // Rendered directly, so no parent decides focus; style without
            // the inverted focus state (same as the skills modal).
            MenuOption tab_option = MenuOption::HorizontalAnimated();
            tab_option.entries_option.transform = [](const EntryState& state) {
                Element e = text(state.label);
                return state.active ? e | bold : e | dim;
            };
            _tabs = Menu(&_tab_names, &_tab, std::move(tab_option));
        }

        Element OnRender() override
        {
            Elements rows = modal_header("Usage");
            rows.push_back(_tabs->Render());
            rows.push_back(separatorEmpty());
            const int width = std::max(40, Terminal::Size().dimx - 12);
            if (_tab == 1 && !_summary) {
                _summary = summarize_saved_usage(_state->sessions->sessions());
            }
            Element body = _tab == 0 ? render_session_tab(*_session, width)
                                     : render_total_tab(*_summary, width);
            rows.push_back(scroll_viewport(std::move(body), _scroll));
            rows.push_back(separatorEmpty());
            rows.push_back(hint_bar("Tab switch · ↑↓ scroll · Esc close"));
            return vbox(std::move(rows)) | xflex;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::Escape) {
                imza::close_modal(*_state);
                return true;
            }
            if (event == Event::Tab || event == Event::TabReverse) {
                _tab    = _tab == 0 ? 1 : 0;
                _scroll = { };
                return true;
            }
            return scroll_viewport_event(_scroll, event);
        }

        void OnAnimation(animation::Params& params) override
        {
            if (_tabs) {
                _tabs->OnAnimation(params);
            }
        }

    private:
        std::shared_ptr<ApplicationState> _state;
        std::shared_ptr<Session> _session;
        std::vector<std::string> _tab_names;
        Component _tabs;
        int _tab = 0;
        // Loaded once when the Total tab first opens.
        std::optional<GlobalUsageSummary> _summary;
        ScrollView _scroll;
    };

} // namespace

ftxui::Component make_usage(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<UsageView>(state);
}

} // namespace imza
