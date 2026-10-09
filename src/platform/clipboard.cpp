#include "platform/clipboard.h"

#include <chrono>

#include "platform/command_runner.h"

namespace imza {

bool copy_to_clipboard(std::string_view clipboard_tool, std::string_view text)
{
    if (text.empty() || clipboard_tool.empty()) {
        return false;
    }
    // Clipboard tools fork a daemon that owns the selection and inherits the
    // runner's output pipes; without this redirect the daemon holds the pipe
    // open forever and the capture reader never sees EOF.
#ifdef _WIN32
    const std::string command = std::string(clipboard_tool) + " >NUL 2>&1";
#else
    const std::string command
        = std::string(clipboard_tool) + " >/dev/null 2>&1";
#endif
    const CommandResult result
        = run_command(command, std::chrono::seconds { 2 }, { }, text);
    return command_ok(result);
}

} // namespace imza
