#include <doctest/doctest.h>

#include "tools/file_ops.h"

#include <string>
#include <vector>

#include "common/util.h"

namespace {

std::string kinds_of(const imza::DiffView& diff)
{
    std::string out;
    for (const imza::DiffRow& row : diff.rows) {
        switch (row.kind) {
        case imza::DiffRow::Kind::SAME: out += 'S'; break;
        case imza::DiffRow::Kind::REMOVE: out += '-'; break;
        case imza::DiffRow::Kind::ADD: out += '+'; break;
        case imza::DiffRow::Kind::SKIP: out += '.'; break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("diff of unchanged files is empty")
{
    const auto lines          = imza::split_lines("same\n");
    const imza::DiffView diff = imza::make_diff_view("a.txt", lines, lines);
    CHECK(diff.rows.empty());
}

TEST_CASE("prepended lines localize to one hunk instead of a full rewrite")
{
    std::vector<std::string> old_lines;
    for (int i = 1; i <= 10; ++i) {
        old_lines.push_back("line " + std::to_string(i));
    }
    std::vector<std::string> new_lines = { "new-1", "new-2" };
    new_lines.insert(new_lines.end(), old_lines.begin(), old_lines.end());
    const imza::DiffView diff
        = imza::make_diff_view("a.txt", old_lines, new_lines);
    CHECK(kinds_of(diff) == "++SSS.");
}

TEST_CASE("two distant edits produce two hunks with elision between")
{
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    for (int i = 1; i <= 20; ++i) {
        const std::string line = "line " + std::to_string(i);
        old_lines.push_back(line);
        if (i == 2) {
            new_lines.push_back("changed");
        } else if (i == 19) {
            new_lines.push_back("tail");
        } else {
            new_lines.push_back(line);
        }
    }
    const imza::DiffView diff
        = imza::make_diff_view("a.txt", old_lines, new_lines);
    const std::string kinds = kinds_of(diff);
    // Each hunk emits its REMOVE row before its ADD rows.
    CHECK(kinds.find("-+") != std::string::npos);
    CHECK(kinds.find("+-") == std::string::npos);
    // Gap of 16 lines: 3 context on each side, 10 elided between.
    std::size_t skips = 0;
    for (const imza::DiffRow& row : diff.rows) {
        if (row.kind != imza::DiffRow::Kind::SKIP) {
            continue;
        }
        ++skips;
        CHECK(row.left_no == row.right_no);
        REQUIRE(row.left_no.has_value());
        CHECK(*row.left_no == 6);
        CHECK(row.left.find("10 unchanged line(s)") != std::string::npos);
    }
    CHECK(skips == 1);
}

TEST_CASE("adjacent edits merge into one hunk")
{
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    for (int i = 1; i <= 10; ++i) {
        const std::string line = "line " + std::to_string(i);
        old_lines.push_back(line);
        new_lines.push_back(i == 2 || i == 5 ? "x" : line);
    }
    const imza::DiffView diff
        = imza::make_diff_view("a.txt", old_lines, new_lines);
    std::size_t skips = 0;
    for (const imza::DiffRow& row : diff.rows) {
        skips += row.kind == imza::DiffRow::Kind::SKIP ? 1 : 0;
    }
    CHECK(skips == 1);
}

TEST_CASE("moved block anchors on its unique lines")
{
    const imza::DiffView diff
        = imza::make_diff_view("a.txt", imza::split_lines("a\nb\nc\nd\ne\n"),
            imza::split_lines("c\nd\nb\na\ne\n"));
    // The unstitched prefix (a,b) relocates; the anchored c,d,e chain
    // stays put.
    std::size_t moves = 0;
    for (const imza::DiffRow& row : diff.rows) {
        if (row.kind == imza::DiffRow::Kind::ADD
            || row.kind == imza::DiffRow::Kind::REMOVE) {
            const bool moved = row.left == "a" || row.left == "b"
                || row.right == "a" || row.right == "b";
            CHECK(moved);
            ++moves;
        }
    }
    CHECK(moves == 4);
}

TEST_CASE("disjoint alphabets collapse to a single replace hunk")
{
    const imza::DiffView diff = imza::make_diff_view(
        "a.txt", imza::split_lines("a\nb\nc\n"), imza::split_lines("x\ny\n"));
    CHECK(kinds_of(diff) == "---++");
}

TEST_CASE("anchor-free regions beyond the Myers cap collapse to one hunk")
{
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    for (int i = 0; i < 3000; ++i) {
        old_lines.push_back("old " + std::to_string(i));
        new_lines.push_back("new " + std::to_string(i));
    }
    const imza::DiffView diff
        = imza::make_diff_view("a.txt", old_lines, new_lines);
    // One replace hunk: every old line removed, every new line added.
    const std::string kinds = kinds_of(diff);
    CHECK(kinds.find("-") == 0);
    CHECK(kinds.find("+") == 3000);
}

TEST_CASE("large identical head and tail trim to a small middle hunk")
{
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    for (int i = 0; i < 5000; ++i) {
        const std::string line = "line " + std::to_string(i);
        old_lines.push_back(line);
        new_lines.push_back(i == 2500 ? "changed" : line);
    }
    const imza::DiffView diff
        = imza::make_diff_view("a.txt", old_lines, new_lines);
    std::size_t removes = 0;
    std::size_t adds    = 0;
    for (const imza::DiffRow& row : diff.rows) {
        removes += row.kind == imza::DiffRow::Kind::REMOVE ? 1 : 0;
        adds += row.kind == imza::DiffRow::Kind::ADD ? 1 : 0;
    }
    CHECK(removes == 1);
    CHECK(adds == 1);
}