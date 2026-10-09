#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace imza {

#ifdef _WIN32

// Windows process APIs take UTF-16; these convert at the boundary so the rest
// of the platform layer speaks UTF-8 like every other domain.
inline std::wstring to_wide(const std::string& text)
{
    if (text.empty()) {
        return L"";
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        out.data(), needed);
    return out;
}

#endif

} // namespace imza
