#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ursa {

inline std::string env_or_empty(const char* key)
{
#ifdef _WIN32
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, key) != 0 || buf == nullptr) {
        return "";
    }
    std::string value(buf);
    free(buf);
    return value;
#else
    const char* value = std::getenv(key);
    return value != nullptr ? std::string(value) : "";
#endif
}

// Appends '.' unless the text already ends with sentence punctuation.
inline std::string ensure_sentence_end(std::string text)
{
    if (!text.empty() && !text.ends_with('.') && !text.ends_with('!')
        && !text.ends_with('?') && !text.ends_with("…")) {
        text += '.';
    }
    return text;
}

inline std::string join_lines(const std::vector<std::string>& lines)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

inline std::string join_lines(
    const std::vector<std::string>& lines, std::size_t begin, std::size_t end)
{
    std::string out;
    for (std::size_t i = begin; i <= end && i < lines.size(); ++i) {
        if (!out.empty()) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

inline std::string format_local_time(const char* fmt)
{
    const auto now          = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm local { };
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, fmt);
    return out.str();
}

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

// Longest prefix of `text` that is at most `max_bytes` bytes and does not
// end in the middle of a UTF-8 sequence.
inline std::string_view truncate_utf8(
    std::string_view text, std::size_t max_bytes)
{
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut);
}

// Start index of the whitespace-delimited token ending at `cursor`.
inline std::size_t word_begin(std::string_view text, std::size_t cursor)
{
    std::size_t begin = std::min(cursor, text.size());
    while (begin > 0
        && !std::isspace(static_cast<unsigned char>(text[begin - 1]))) {
        --begin;
    }
    return begin;
}

// End index (exclusive) of a `$name` mention body whose '$' sits at `hash`.
// The body spans [A-Za-z0-9_-]; an empty body ends right after the '$'.
inline std::size_t mention_end(std::string_view text, std::size_t hash)
{
    std::size_t end = hash + 1;
    while (end < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[end]);
        if (!std::isalnum(c) && c != '-' && c != '_') {
            break;
        }
        ++end;
    }
    return end;
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

inline std::string home_dir()
{
    return env_or_empty(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
}

} // namespace ursa
