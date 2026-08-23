#pragma once

#include <cctype>
#include <string>
#include <string_view>

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

} // namespace ursa
