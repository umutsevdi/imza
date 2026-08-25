#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ursa {

inline std::string_view trim(std::string_view s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

inline std::string to_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

inline std::string strip_slash(std::string_view base)
{
    std::string out(base);
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

inline std::size_t count_lines(std::string_view text)
{
    if (text.empty()) {
        return 0;
    }
    std::size_t n = 1;
    for (const char c : text) {
        if (c == '\n') {
            ++n;
        }
    }
    return n;
}

inline std::string take_lines(std::string_view text, std::size_t max)
{
    std::string out;
    std::size_t lines = 0;
    for (const char c : text) {
        out += c;
        if (c == '\n' && ++lines >= max) {
            break;
        }
    }
    return out;
}

inline std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines;
    }
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        lines.emplace_back(text.substr(start));
    }
    return lines;
}

template <typename Container>
std::string join(const Container& items, std::string_view sep)
{
    std::string out;
    bool first = true;
    for (const auto& item : items) {
        if (!first) {
            out += sep;
        }
        first = false;
        out += item;
    }
    return out;
}

} // namespace ursa
