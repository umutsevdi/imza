#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ursa {

struct DiffRow {
    enum class Kind { SAME, REMOVE, ADD };
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

} // namespace ursa
