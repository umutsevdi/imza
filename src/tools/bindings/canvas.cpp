#include "tools/bindings.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    constexpr std::size_t MAX_RUN_CANVASES = 16;
    constexpr std::size_t MAX_SERIES       = 8;
    constexpr std::size_t MAX_POINTS       = 512;
    constexpr std::size_t MAX_CATEGORIES   = 64;
    constexpr std::size_t MAX_GRID_EDGE    = 64;

    // String field of the table at `index`; empty when absent. Fills
    // `error` when present with a non-string value. Stack-neutral.
    void option_string(lua_State* L, int index, const char* key,
        std::string& out, std::string& error)
    {
        lua_getfield(L, index, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            error = std::string("'") + key + "' must be a string";
            return;
        }
        std::size_t size  = 0;
        const char* value = lua_tolstring(L, -1, &size);
        out.assign(value, size);
        lua_pop(L, 1);
    }

    // Array of finite numbers at absolute `index`. Stack-neutral; fills
    // `error` and leaves `out` untouched on failure.
    void number_array(lua_State* L, int index, std::size_t min, std::size_t cap,
        const std::string& what, std::vector<double>& out, std::string& error)
    {
        if (!lua_istable(L, index)) {
            error = what + " must be an array of numbers";
            return;
        }
        const std::size_t n = lua_rawlen(L, index);
        if (n < min || n > cap) {
            error = what + " needs " + std::to_string(min) + ".."
                + std::to_string(cap) + " numbers";
            return;
        }
        std::vector<double> values;
        values.reserve(n);
        for (std::size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, index, static_cast<lua_Integer>(i));
            const bool numeric = lua_isnumber(L, -1) != 0;
            const double value = numeric ? lua_tonumber(L, -1) : 0.0;
            lua_pop(L, 1);
            if (!numeric || !std::isfinite(value)) {
                error = what + " entry " + std::to_string(i)
                    + " must be a finite number";
                return;
            }
            values.push_back(value);
        }
        out = std::move(values);
    }

    // Stores a validated chart, logs the call, and answers the boolean
    // success the model sees instead of rendered characters.
    void emit_canvas(lua_State* L, const std::string& binding, CanvasView view)
    {
        const std::string title = view.title;
        run_of(L)->canvases.push_back(std::move(view));
        record_call(L, binding, title, true);
        lua_pushboolean(L, 1);
    }

    bool canvas_room(lua_State* L, const std::string& binding)
    {
        if (run_of(L)->canvases.size() < MAX_RUN_CANVASES) {
            return true;
        }
        binding_error(L, binding + ": too many charts in one run");
        return false;
    }

    int binding_canvas_line(lua_State* L)
    {
        const std::string binding = "canvas.line";
        luaL_checktype(L, 1, LUA_TTABLE);
        std::string error;
        CanvasView view;
        view.kind = CanvasView::Kind::LINE;
        option_string(L, 1, "title", view.title, error);
        std::string label;
        option_string(L, 1, "label", label, error);

        lua_getfield(L, 1, "data");
        const int data      = lua_gettop(L);
        const bool is_table = lua_istable(L, data) != 0;
        const std::size_t n = is_table ? lua_rawlen(L, data) : 0;
        if (n == 0 || n > MAX_SERIES) {
            error = error.empty()
                ? "'data' needs 1.." + std::to_string(MAX_SERIES) + " entries"
                : error;
        }
        // Shape from the first entry: number → one flat series, table → a
        // CanvasSeries list.
        bool flat = true;
        if (error.empty() && n > 0) {
            lua_rawgeti(L, data, 1);
            flat = lua_type(L, -1) != LUA_TTABLE;
            lua_pop(L, 1);
        }
        if (error.empty() && flat) {
            std::vector<double> values;
            number_array(
                L, data, 1, MAX_POINTS, binding + ": 'data'", values, error);
            if (error.empty()) {
                view.series.push_back({ std::move(label), std::move(values) });
            }
        }
        for (std::size_t i = 1; error.empty() && i <= n && !flat; ++i) {
            lua_rawgeti(L, data, static_cast<lua_Integer>(i));
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                error = "'data' entry " + std::to_string(i)
                    + " must be a CanvasSeries table";
                break;
            }
            std::string series_label;
            option_string(L, -1, "label", series_label, error);
            lua_getfield(L, -1, "values");
            std::vector<double> values;
            if (error.empty()) {
                number_array(L, -1, 1, MAX_POINTS,
                    binding + ": series " + std::to_string(i) + " 'values'",
                    values, error);
            }
            lua_pop(L, 2);
            if (error.empty()) {
                view.series.push_back(
                    { std::move(series_label), std::move(values) });
            }
        }
        lua_pop(L, 1);
        if (!error.empty()) {
            return binding_error(L, binding + ": " + error);
        }
        if (!canvas_room(L, binding)) {
            return 2;
        }
        emit_canvas(L, binding, std::move(view));
        return 1;
    }

    // Shared bar/pie parsing: data is a list of {label, value} points.
    // Pie slices must be positive; bars may be zero or negative.
    int binding_canvas_categories(lua_State* L, CanvasView::Kind kind,
        const std::string& binding, bool positive)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        std::string error;
        CanvasView view;
        view.kind = kind;
        option_string(L, 1, "title", view.title, error);

        lua_getfield(L, 1, "data");
        const int data = lua_gettop(L);
        if (!lua_istable(L, data)) {
            lua_pop(L, 1);
            error = error.empty() ? "needs a 'data' array" : error;
        } else {
            const std::size_t n = lua_rawlen(L, data);
            if (n == 0 || n > MAX_CATEGORIES) {
                error = error.empty() ? "'data' needs 1.."
                        + std::to_string(MAX_CATEGORIES) + " points"
                                      : error;
            }
            for (std::size_t i = 1; error.empty() && i <= n; ++i) {
                lua_rawgeti(L, data, static_cast<lua_Integer>(i));
                if (!lua_istable(L, -1)) {
                    lua_pop(L, 1);
                    error = "'data' point " + std::to_string(i)
                        + " must be a table";
                    break;
                }
                std::string label;
                option_string(L, -1, "label", label, error);
                lua_getfield(L, -1, "value");
                const bool numeric = lua_isnumber(L, -1) != 0;
                const double value = numeric ? lua_tonumber(L, -1) : 0.0;
                lua_pop(L, 2);
                if (!error.empty()) {
                    break;
                }
                if (label.empty() || !numeric || !std::isfinite(value)
                    || (positive && value <= 0)) {
                    error = "'data' point " + std::to_string(i)
                        + " needs a 'label' string and a finite "
                        + (positive ? "positive " : "") + "'value' number";
                    break;
                }
                view.series.push_back({ std::move(label), { value } });
            }
            lua_pop(L, 1);
        }
        if (!error.empty()) {
            return binding_error(L, binding + ": " + error);
        }
        if (!canvas_room(L, binding)) {
            return 2;
        }
        emit_canvas(L, binding, std::move(view));
        return 1;
    }

    int binding_canvas_bar(lua_State* L)
    {
        return binding_canvas_categories(
            L, CanvasView::Kind::BAR, "canvas.bar", false);
    }

    int binding_canvas_pie(lua_State* L)
    {
        return binding_canvas_categories(
            L, CanvasView::Kind::PIE, "canvas.pie", true);
    }

    int binding_canvas_surface(lua_State* L)
    {
        const std::string binding = "canvas.surface";
        luaL_checktype(L, 1, LUA_TTABLE);
        std::string error;
        CanvasView view;
        view.kind = CanvasView::Kind::SURFACE;
        option_string(L, 1, "title", view.title, error);

        lua_getfield(L, 1, "data");
        const int data = lua_gettop(L);
        if (!lua_istable(L, data)) {
            lua_pop(L, 1);
            error = error.empty() ? "needs a 'data' matrix" : error;
        } else {
            const std::size_t rows = lua_rawlen(L, data);
            if (rows < 2 || rows > MAX_GRID_EDGE) {
                error = error.empty() ? "'data' needs 2.."
                        + std::to_string(MAX_GRID_EDGE) + " rows"
                                      : error;
            }
            std::size_t columns = 0;
            for (std::size_t r = 1; error.empty() && r <= rows; ++r) {
                lua_rawgeti(L, data, static_cast<lua_Integer>(r));
                std::vector<double> row;
                number_array(L, -1, 1, MAX_GRID_EDGE,
                    binding + ": row " + std::to_string(r), row, error);
                lua_pop(L, 1);
                if (!error.empty()) {
                    break;
                }
                if (columns == 0) {
                    if (row.size() < 2) {
                        error = "'data' rows need 2.."
                            + std::to_string(MAX_GRID_EDGE) + " columns";
                        break;
                    }
                    columns = row.size();
                } else if (row.size() != columns) {
                    error = "row " + std::to_string(r) + " must have "
                        + std::to_string(columns) + " columns";
                    break;
                }
                view.grid.push_back(std::move(row));
            }
            lua_pop(L, 1);
        }
        if (!error.empty()) {
            return binding_error(L, binding + ": " + error);
        }
        if (!canvas_room(L, binding)) {
            return 2;
        }
        emit_canvas(L, binding, std::move(view));
        return 1;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "line",
            binding_canvas_line,
            R"desc((options: { title?: string, label?: string, data }) => true
Renders a line chart directly in the chat, one polyline per series with
x as the point index.
`data` is a list of numbers or a list of CanvasSeries tables; `label`
names a flat data series.
Returns true once the chart is accepted for rendering; nil, Err on
invalid data.)desc",
        },
        {
            "bar",
            binding_canvas_bar,
            R"desc((options: { title?: string, data: CanvasPoint[] }) => true
Renders a vertical bar chart directly in the chat, one bar per point.
Returns true once the chart is accepted for rendering; nil, Err on
invalid data.)desc",
        },
        {
            "pie",
            binding_canvas_pie,
            R"desc((options: { title?: string, data: CanvasPoint[] }) => true
Renders a pie chart directly in the chat, one slice per point.
Returns true once the chart is accepted for rendering; nil, Err on
invalid data.)desc",
        },
        {
            "surface",
            binding_canvas_surface,
            R"desc((options: { title?: string, data: number[][] }) => true
Renders a wireframe surface directly in the chat: `data` is a
rectangular matrix of z values, row-major.
Returns true once the chart is accepted for rendering; nil, Err on
invalid data.)desc",
        },
    };

} // namespace

std::span<const LuaMethod> canvas_lua_methods() { return BINDINGS; }

void register_canvas(LuaState& state)
{
    static constexpr std::string_view types[] = {
        "CanvasSeries = { label: string, values: number[] }",
        "CanvasPoint = { label: string, value: number }",
    };
    state.register_module({ false, "canvas",
        "Charts rendered inline in the chat.", types, canvas_lua_methods() });
}

} // namespace imza
