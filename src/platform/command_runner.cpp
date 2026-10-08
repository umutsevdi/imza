#include "platform/command_runner.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
/* Do not change the order. Windows API is cursed */
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace imza {

namespace {

#ifdef _WIN32

    std::wstring to_wide(const std::string& s)
    {
        if (s.empty()) {
            return L"";
        }
        const int needed = MultiByteToWideChar(
            CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
            out.data(), needed);
        return out;
    }

    CommandResult run_windows(const std::string& command,
        std::chrono::seconds timeout, CommandResult result,
        const std::filesystem::path& working_directory,
        std::string_view stdin_data)
    {
        std::wstring cmdline = L"cmd.exe /c " + to_wide(command);

        SECURITY_ATTRIBUTES sa { };
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE out_read   = nullptr;
        HANDLE out_write  = nullptr;
        if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
            return result;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

        HANDLE in_read  = nullptr;
        HANDLE in_write = nullptr;
        const bool has_stdin
            = !stdin_data.empty() && CreatePipe(&in_read, &in_write, &sa, 0);
        if (has_stdin) {
            SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        } else {
            // Children must not share the console input with the UI's
            // reader; the two would race for the user's keystrokes.
            in_read = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                OPEN_EXISTING, 0, nullptr);
            if (in_read == INVALID_HANDLE_VALUE) {
                CloseHandle(out_read);
                CloseHandle(out_write);
                in_read = nullptr;
                return result;
            }
        }

        STARTUPINFOW si { };
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = out_write;
        si.hStdError  = out_write;
        si.hStdInput  = in_read;

        PROCESS_INFORMATION pi { };
        const std::wstring wide_directory = working_directory.empty()
            ? std::wstring { }
            : working_directory.wstring();
        if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
                nullptr,
                wide_directory.empty() ? nullptr : wide_directory.c_str(), &si,
                &pi)) {
            CloseHandle(out_read);
            CloseHandle(out_write);
            CloseHandle(in_read);
            if (has_stdin) {
                CloseHandle(in_write);
            }
            return result;
        }
        CloseHandle(out_write);
        CloseHandle(in_read);

        std::string output;
        std::thread reader([&out_read, &output] {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(out_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
                output.append(buf, static_cast<std::size_t>(n));
            }
        });

        std::thread stdin_writer;
        if (has_stdin) {
            stdin_writer
                = std::thread([data = std::string(stdin_data), in_write] {
                      const char* cursor    = data.data();
                      std::size_t remaining = data.size();
                      while (remaining > 0) {
                          DWORD written = 0;
                          if (!WriteFile(in_write, cursor,
                                  static_cast<DWORD>(std::min<std::size_t>(
                                      remaining, 0x7fffffff)),
                                  &written, nullptr)
                              || written == 0) {
                              break;
                          }
                          cursor += written;
                          remaining -= written;
                      }
                      CloseHandle(in_write);
                  });
        }

        const DWORD ms   = timeout.count() > 0x7fffffff / 1000
            ? INFINITE
            : static_cast<DWORD>(timeout.count() * 1000);
        const DWORD wait = WaitForSingleObject(pi.hProcess, ms);
        if (wait == WAIT_TIMEOUT) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, INFINITE);
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = static_cast<int>(code);

        reader.join();
        if (stdin_writer.joinable()) {
            stdin_writer.join();
        }
        CloseHandle(out_read);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        result.spawned = true;
        result.output  = std::move(output);
        return result;
    }

    CommandResult run_windows_attached(
        const std::string& command, CommandResult result)
    {
        std::wstring cmdline = to_wide(command);
        STARTUPINFOW startup { };
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process { };
        if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
                nullptr, nullptr, &startup, &process)) {
            return result;
        }
        result.spawned = true;
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(process.hProcess, &exit_code);
        result.exit_code = static_cast<int>(exit_code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        return result;
    }

#else

    CommandResult run_posix(const std::string& command,
        std::chrono::seconds timeout, CommandResult result,
        const std::filesystem::path& working_directory,
        std::string_view stdin_data)
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            return result;
        }
        int stdin_pipefd[2];
        const bool has_stdin = !stdin_data.empty() && pipe(stdin_pipefd) == 0;
        const pid_t pid      = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            if (has_stdin) {
                close(stdin_pipefd[0]);
                close(stdin_pipefd[1]);
            }
            return result;
        }
        if (pid == 0) {
            // 126 marks "cannot run" (here: the directory vanished between
            // validation and exec); the binding pre-validates, so this only
            // covers a TOCTOU race.
            if (!working_directory.empty()
                && chdir(working_directory.c_str()) != 0) {
                _exit(126);
            }
            setpgid(0, 0);
            if (has_stdin) {
                dup2(stdin_pipefd[0], STDIN_FILENO);
                close(stdin_pipefd[0]);
                close(stdin_pipefd[1]);
            } else {
                // The child must never share the terminal stdin with the
                // UI's reader: the two would race for the user's
                // keystrokes, splitting or swallowing escape sequences.
                const int devnull = open("/dev/null", O_RDONLY);
                if (devnull < 0) {
                    _exit(126);
                }
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            execl("/bin/sh", "sh", "-c", command.c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }

        close(pipefd[1]);
        const int read_end = pipefd[0];

        std::thread stdin_writer;
        int stdin_write_end = -1;
        if (has_stdin) {
            close(stdin_pipefd[0]);
            stdin_write_end = stdin_pipefd[1];
            // An early child exit makes the write raise SIGPIPE; blocking it
            // here turns that into a returnable EPIPE so the caller keeps
            // running. write() unblocks once the child is reaped and the
            // descriptor closes.
            stdin_writer = std::thread([stdin_data, fd = stdin_write_end] {
                sigset_t mask;
                sigemptyset(&mask);
                sigaddset(&mask, SIGPIPE);
                pthread_sigmask(SIG_BLOCK, &mask, nullptr);
                const char* cursor    = stdin_data.data();
                std::size_t remaining = stdin_data.size();
                while (remaining > 0) {
                    const ssize_t written = write(fd, cursor, remaining);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                    cursor += written;
                    remaining -= static_cast<std::size_t>(written);
                }
                close(fd);
            });
        }

        std::string output;
        std::thread reader([read_end, &output] {
            char buf[4096];
            ssize_t n = 0;
            while ((n = read(read_end, buf, sizeof(buf))) > 0) {
                output.append(buf, static_cast<std::size_t>(n));
            }
        });

        std::future<int> reaper = std::async(std::launch::async, [pid] {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }
            return -1;
        });

        if (reaper.wait_for(timeout) == std::future_status::timeout) {
            result.timed_out = true;
            result.exit_code = 124;
            killpg(pid, SIGKILL);
            reaper.get();
        } else {
            result.exit_code = reaper.get();
        }

        reader.join();
        close(read_end);
        if (stdin_writer.joinable()) {
            stdin_writer.join();
        }

        result.spawned = true;
        result.output  = std::move(output);
        return result;
    }

    CommandResult run_posix_attached(
        const std::string& command, CommandResult result)
    {
        const pid_t pid = fork();
        if (pid < 0) {
            return result;
        }
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", command.c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            return result;
        }
        result.spawned = true;
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
        } else {
            result.exit_code = -1;
        }
        return result;
    }

#endif

} // namespace

std::string shell_quote(const std::filesystem::path& path)
{
#ifdef _WIN32
    return "\"" + path.string() + "\"";
#else
    std::string quoted = "'";
    for (const char character : path.string()) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    return quoted + "'";
#endif
}

CommandResult run_command(const std::string& command,
    std::chrono::seconds timeout,
    const std::filesystem::path& working_directory, std::string_view stdin_data)
{
    CommandResult result;
    if (command.empty() || timeout < std::chrono::seconds(0)) {
        return result;
    }
#ifdef _WIN32
    return run_windows(
        command, timeout, std::move(result), working_directory, stdin_data);
#else
    return run_posix(
        command, timeout, std::move(result), working_directory, stdin_data);
#endif
}

CommandResult run_attached_command(const std::string& command)
{
    CommandResult result;
    if (command.empty()) {
        return result;
    }
#ifdef _WIN32
    return run_windows_attached(command, std::move(result));
#else
    return run_posix_attached(command, std::move(result));
#endif
}

bool open_browser(std::string_view url)
{
    if (url.empty()) {
        return false;
    }
#ifdef _WIN32
    const std::wstring wide = to_wide(std::string(url));
    return reinterpret_cast<std::intptr_t>(ShellExecuteW(
               nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL))
        > 32;
#else
    const std::string target(url);
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
#ifdef __APPLE__
        execlp("open", "open", target.c_str(), static_cast<char*>(nullptr));
#else
        execlp("xdg-open", "xdg-open", target.c_str(),
            static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    }).detach();
    return true;
#endif
}

} // namespace imza
