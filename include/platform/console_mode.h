#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

namespace imza {

#ifdef _WIN32

// Clears DISABLE_NEWLINE_AUTO_RETURN on the stdout console and restores
// the previous mode on destruction. std::println writes a bare LF through
// WriteConsoleW; a console left in VT mode (shell, WSL, a prior TUI) would
// staircase it. The mode is a shared console property, so it is restored
// rather than left changed.
class ConsoleModeScope {
public:
    ConsoleModeScope();
    ~ConsoleModeScope();

    ConsoleModeScope(const ConsoleModeScope&)            = delete;
    ConsoleModeScope& operator=(const ConsoleModeScope&) = delete;

private:
    HANDLE _stdout  = nullptr;
    DWORD _original = 0;
    bool _restore   = false;
};

#endif

} // namespace imza