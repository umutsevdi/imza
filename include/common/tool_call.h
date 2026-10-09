#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/types.h"

namespace imza {

struct DiffRow {
    enum class Kind { SAME, REMOVE, ADD, SKIP };
    Kind kind = Kind::SAME;
    std::optional<std::size_t> left_no;
    std::optional<std::size_t> right_no;
    std::string left;
    std::string right;
};

struct DiffView {
    std::string file;
    std::vector<DiffRow> rows;
};

struct CanvasSeries {
    std::string label;
    std::vector<double> values;

    bool operator==(const CanvasSeries&) const = default;
};

// A chart the lua canvas module emitted for inline chat rendering. The
// transcript stores this declarative data; the UI draws it with an FTXUI
// canvas sized to the current terminal width. Line stores one series per
// plotted line; bar and pie store one entry per labeled value; surface
// stores z values row-major.
struct CanvasView {
    enum class Kind { LINE, BAR, PIE, SURFACE };
    Kind kind = Kind::LINE;
    std::string title;
    std::vector<CanvasSeries> series;
    std::vector<std::vector<double>> grid;

    bool operator==(const CanvasView&) const = default;
};

struct ShellExit {
    int code;
};

struct ShellTimeout {
    std::chrono::seconds duration;
};

using ShellStatus = std::variant<ShellExit, ShellTimeout>;

// One sandbox-binding call a lua script made: what it touched and whether
// the gate allowed it. UI-only; the model transcript never includes it.
struct LuaBindingCall {
    std::string binding;
    std::string target;
    bool ok = true;

    bool operator==(const LuaBindingCall&) const = default;
};

enum class ToolDecision { ACCEPT_ONCE, ACCEPT_FOR_SESSION, REJECT };

struct ToolVerdict {
    ToolDecision decision = ToolDecision::REJECT;
    std::string reason;
};

struct TodoItem {
    enum class Status { PENDING, IN_PROGRESS, COMPLETED, CANCELLED };
    std::string content;
    Status status = Status::PENDING;

    bool operator==(const TodoItem&) const = default;
};

struct TodoList {
    std::vector<TodoItem> items;

    bool operator==(const TodoList&) const = default;
};

} // namespace imza
