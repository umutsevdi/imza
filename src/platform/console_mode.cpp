#include "platform/console_mode.h"

#ifdef _WIN32

namespace imza {

ConsoleModeScope::ConsoleModeScope()
{
    _stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (_stdout == nullptr || _stdout == INVALID_HANDLE_VALUE
        || !GetConsoleMode(_stdout, &_original)) {
        // Not a console (redirected/piped): nothing to save or clear.
        return;
    }
    const DWORD cleaned = _original & ~0x0008u; // DISABLE_NEWLINE_AUTO_RETURN
    _restore            = SetConsoleMode(_stdout, cleaned) != 0;
}

ConsoleModeScope::~ConsoleModeScope()
{
    if (_restore) {
        SetConsoleMode(_stdout, _original);
    }
}

} // namespace imza

#endif