#include "command_runner.h"

#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ursa {

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
    std::chrono::seconds timeout, CommandResult result)
{
    std::wstring cmdline = L"cmd.exe /c " + to_wide(command);

    SECURITY_ATTRIBUTES sa { };
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        return result;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si { };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_write;
    si.hStdError = out_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi { };
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
            nullptr, nullptr, &si, &pi)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return result;
    }
    CloseHandle(out_write);

    std::string output;
    std::thread reader([&out_read, &output] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(out_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
            output.append(buf, static_cast<std::size_t>(n));
        }
    });

    const DWORD ms = timeout.count() > 0x7fffffff / 1000
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
    CloseHandle(out_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    result.spawned = true;
    result.output = std::move(output);
    return result;
}

#else

CommandResult run_posix(const std::string& command,
    std::chrono::seconds timeout, CommandResult result)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(pipefd[1]);
    const int read_end = pipefd[0];

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

    result.spawned = true;
    result.output = std::move(output);
    return result;
}

#endif

} // namespace

CommandResult run_command(const std::string& command, std::chrono::seconds timeout)
{
    CommandResult result;
    if (command.empty() || timeout < std::chrono::seconds(0)) {
        return result;
    }
#ifdef _WIN32
    return run_windows(command, timeout, std::move(result));
#else
    return run_posix(command, timeout, std::move(result));
#endif
}

} // namespace ursa
