#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <map>
#include <vector>
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

// The UTF-16 environment block for CreateProcess, in its double-NUL-terminated
// form. Empty `env` returns an empty block, meaning "inherit": the inherited
// entries are copied first and the overrides substituted in, because dropping
// the rest would strip PATH and SystemRoot and the child could not start.
// "=C:=..." drive-directory entries have no name to override and are kept.
inline std::vector<wchar_t> windows_environment_block(
    const std::map<std::string, std::string>& env)
{
    std::vector<wchar_t> block;
    if (env.empty()) {
        return block;
    }
    std::vector<std::wstring> overridden;
    overridden.reserve(env.size());
    for (const auto& [name, value] : env) {
        overridden.push_back(to_wide(name));
    }
    std::vector<std::wstring> entries;
    if (wchar_t* inherited = GetEnvironmentStringsW()) {
        for (const wchar_t* cursor = inherited; *cursor != L'\0';
            cursor += std::wcslen(cursor) + 1) {
            const std::wstring entry(cursor);
            const std::size_t equals = entry.find(L'=');
            const std::wstring name
                = equals == 0 ? std::wstring { } : entry.substr(0, equals);
            if (name.empty()
                || std::find(overridden.begin(), overridden.end(), name)
                    == overridden.end()) {
                entries.push_back(entry);
            }
        }
        FreeEnvironmentStringsW(inherited);
    }
    for (const auto& [name, value] : env) {
        entries.push_back(to_wide(name + "=" + value));
    }
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

#endif

} // namespace imza
