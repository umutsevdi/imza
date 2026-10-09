#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace imza {

// True when `target` equals `root` or lies underneath it. Both paths are
// expected to be canonical; an empty root contains nothing.
inline bool path_within(
    const std::filesystem::path& root, const std::filesystem::path& target)
{
    if (root.empty()) {
        return false;
    }
    const std::filesystem::path relative = target.lexically_relative(root);
    return target == root || (!relative.empty() && *relative.begin() != "..");
}

// Paths and JSON strings meet as UTF-8 text; `std::filesystem::path`
// conversions via `string()` use the system code page on Windows and would
// corrupt non-ASCII names.
inline std::string utf8_from_path(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path path_from_utf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

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

inline std::string join_lines(
    const std::vector<std::string>& lines, bool trailing_newline)
{
    std::string out = join(lines, "\n");
    if (!lines.empty() && trailing_newline) {
        out += '\n';
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

// Expands ${VAR} and ${VAR:-default} from the environment; unmatched
// references expand to empty. Text outside references is copied verbatim.
inline std::string expand_env_vars(const std::string& text)
{
    static constexpr std::string_view DEFAULT_SEP = ":-";
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (true) {
        const std::size_t begin = text.find("${", pos);
        if (begin == std::string::npos) {
            out += text.substr(pos);
            return out;
        }
        const std::size_t end = text.find('}', begin + 2);
        if (end == std::string::npos) {
            out += text.substr(pos);
            return out;
        }
        out += text.substr(pos, begin - pos);
        const std::string_view ref(text.data() + begin + 2, end - begin - 2);
        const std::size_t sep = ref.find(DEFAULT_SEP);
        const std::string name(ref.substr(0, sep));
        const std::string fallback = sep == std::string_view::npos
            ? std::string { }
            : std::string(ref.substr(sep + DEFAULT_SEP.size()));
        const std::string value    = env_or_empty(name.c_str());
        out += value.empty() ? fallback : value;
        pos = end + 1;
    }
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

// Cap shared by tool outputs; enforced with truncate_marked.
inline constexpr std::size_t MAX_OUTPUT_BYTES = 64 * 1024;

// Cap for tool text that feeds the model directly (web, mcp, docs).
inline constexpr std::size_t MAX_TOOL_TEXT = 40000;

inline constexpr std::string_view TRUNCATION_MARKER = "\n[truncated]";

// Cut at a UTF-8 boundary and mark when over `cap`.
inline std::string truncate_marked(std::string_view text, std::size_t cap,
    std::string_view marker = TRUNCATION_MARKER)
{
    if (text.size() <= cap) {
        return std::string(text);
    }
    std::string out { truncate_utf8(text, cap) };
    out += marker;
    return out;
}

// Like truncate_marked, but the marker reports the kept size.
inline std::string truncate_with_count(std::string_view text, std::size_t cap)
{
    if (text.size() <= cap) {
        return std::string(text);
    }
    std::string out { truncate_utf8(text, cap) };
    out += "\n[truncated: showing first " + std::to_string(out.size())
        + " of the content]";
    return out;
}

// RFC 3986 unreserved characters; everything else is percent-encoded.
inline std::string percent_encode(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
            || (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '_'
            || u == '~') {
            out.push_back(static_cast<char>(u));
        } else {
            out += '%';
            out += hex[u >> 4];
            out += hex[u & 0xF];
        }
    }
    return out;
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

// Length in bytes of the UTF-8 sequence whose lead byte is `lead`.
inline std::size_t utf8_sequence_length(unsigned char lead)
{
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

// Byte length of the valid UTF-8 sequence starting at `i`, or 0 when the
// bytes there are not one. Validates rather than classifying the lead byte.
inline std::size_t utf8_valid_sequence_length(
    std::string_view text, std::size_t i)
{
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t length       = 0;
    unsigned int code_point  = 0;
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        length     = 2;
        code_point = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length     = 3;
        code_point = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length     = 4;
        code_point = lead & 0x07;
    } else {
        return 0;
    }
    if (i + length > text.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char c = static_cast<unsigned char>(text[i + k]);
        if ((c & 0xC0) != 0x80) {
            return 0;
        }
        code_point = (code_point << 6) | (c & 0x3F);
    }
    if ((length == 2 && code_point < 0x80)
        || (length == 3 && code_point < 0x800)
        || (length == 4 && code_point < 0x10000) || code_point > 0x10FFFF
        || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        return 0;
    }
    return length;
}

// Drops bytes that are not part of any valid UTF-8 sequence. Returns
// nullopt when the input is already valid.
inline std::optional<std::string> strip_invalid_utf8(std::string_view text)
{
    std::optional<std::string> repaired;
    std::size_t copied = 0; // bytes of `text` already appended to *repaired
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const std::size_t length = utf8_valid_sequence_length(text, i);
        if (length != 0) {
            i += length;
            continue;
        }
        if (!repaired) {
            repaired.emplace(text.substr(0, i));
            copied = i;
        }
        repaired->append(text.substr(copied, i - copied));
        copied = i + 1; // drop the invalid byte
        ++i;
    }
    if (repaired) {
        repaired->append(text.substr(copied));
    }
    return repaired;
}

// Number of display columns: one per UTF-8 sequence.
inline std::size_t utf8_width(std::string_view s)
{
    std::size_t w = 0;
    for (std::size_t i = 0; i < s.size();) {
        i += utf8_sequence_length(static_cast<unsigned char>(s[i]));
        ++w;
    }
    return w;
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
    return 1 + static_cast<std::size_t>(std::ranges::count(text, '\n'));
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

// Globally unique, sortable identifier: wall-clock milliseconds plus a random
// 32-bit suffix. Generated once per fresh Session object; a loaded session
// instead adopts its source file's stem as the id so saves continue that file
// in place.
inline std::string unique_session_id()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds
        = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::random_device random;
    const std::uint32_t suffix = static_cast<std::uint32_t>(random());
    std::ostringstream out;
    out << milliseconds << '-' << std::hex << std::setw(8) << std::setfill('0')
        << suffix;
    return out.str();
}

inline std::string base64_encode(std::string_view data)
{
    constexpr std::string_view ALPHABET
        = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);

    for (std::size_t offset = 0; offset < data.size(); offset += 3) {
        const auto first         = static_cast<unsigned char>(data[offset]);
        const auto second        = offset + 1 < data.size()
            ? static_cast<unsigned char>(data[offset + 1])
            : 0;
        const auto third         = offset + 2 < data.size()
            ? static_cast<unsigned char>(data[offset + 2])
            : 0;
        const unsigned int value = (static_cast<unsigned int>(first) << 16)
            | (static_cast<unsigned int>(second) << 8)
            | static_cast<unsigned int>(third);

        encoded.push_back(ALPHABET[(value >> 18) & 0x3f]);
        encoded.push_back(ALPHABET[(value >> 12) & 0x3f]);
        encoded.push_back(
            offset + 1 < data.size() ? ALPHABET[(value >> 6) & 0x3f] : '=');
        encoded.push_back(
            offset + 2 < data.size() ? ALPHABET[value & 0x3f] : '=');
    }
    return encoded;
}

inline std::optional<std::string> base64_decode(std::string_view input)
{
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    if (input.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const int first  = value(input[i]);
        const int second = value(input[i + 1]);
        if (first < 0 || second < 0) {
            return std::nullopt;
        }
        const bool third_padding  = input[i + 2] == '=';
        const bool fourth_padding = input[i + 3] == '=';
        const int third           = third_padding ? 0 : value(input[i + 2]);
        const int fourth          = fourth_padding ? 0 : value(input[i + 3]);
        if (third < 0 || fourth < 0 || (third_padding && !fourth_padding)
            || ((third_padding || fourth_padding) && i + 4 != input.size())) {
            return std::nullopt;
        }
        out += static_cast<char>((first << 2) | (second >> 4));
        if (!third_padding) {
            out += static_cast<char>(((second & 0x0f) << 4) | (third >> 2));
        }
        if (!fourth_padding) {
            out += static_cast<char>(((third & 0x03) << 6) | fourth);
        }
    }
    return out;
}

} // namespace imza
