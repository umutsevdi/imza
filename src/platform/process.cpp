#include "platform/process.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace imza {

namespace {

    // CRLF children read as clean lines.
    void strip_cr(std::string& line)
    {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }

} // namespace

struct Process::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> lines;
    std::string stdout_buffer;
    bool stdout_closed = false;
    std::string stderr_tail;
    std::size_t stderr_cap = 8192;
    std::atomic_bool running { false };
    std::atomic_bool reaped { false };
    std::thread stdout_reader;
    std::thread stderr_reader;

#ifdef _WIN32
    HANDLE process  = nullptr;
    HANDLE job      = nullptr;
    HANDLE stdin_w  = nullptr;
    HANDLE stdout_r = nullptr;
    HANDLE stderr_r = nullptr;
#else
    pid_t pid     = -1;
    int stdin_fd  = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
#endif

    ~Impl()
    {
        if (stdout_reader.joinable()) {
            stdout_reader.join();
        }
        if (stderr_reader.joinable()) {
            stderr_reader.join();
        }
#ifdef _WIN32
        if (stdin_w != nullptr) {
            CloseHandle(stdin_w);
        }
        if (stdout_r != nullptr) {
            CloseHandle(stdout_r);
        }
        if (stderr_r != nullptr) {
            CloseHandle(stderr_r);
        }
        if (process != nullptr) {
            CloseHandle(process);
        }
        if (job != nullptr) {
            CloseHandle(job);
        }
#else
        if (stdin_fd >= 0) {
            ::close(stdin_fd);
        }
        if (stdout_fd >= 0) {
            ::close(stdout_fd);
        }
        if (stderr_fd >= 0) {
            ::close(stderr_fd);
        }
#endif
    }

    void publish_lines(bool flush)
    {
        std::lock_guard lock(mutex);
        std::size_t start = 0;
        while (true) {
            const std::size_t newline = stdout_buffer.find('\n', start);
            if (newline == std::string::npos) {
                break;
            }
            std::string line = stdout_buffer.substr(start, newline - start);
            strip_cr(line);
            lines.push_back(std::move(line));
            start = newline + 1;
        }
        stdout_buffer.erase(0, start);
        if (flush && !stdout_buffer.empty()) {
            strip_cr(stdout_buffer);
            lines.push_back(std::move(stdout_buffer));
            stdout_buffer.clear();
        }
        cv.notify_all();
    }

    void mark_stdout_closed()
    {
        publish_lines(true);
        {
            std::lock_guard lock(mutex);
            stdout_closed = true;
        }
        cv.notify_all();
    }

    void append_stderr(const char* data, std::size_t size)
    {
        std::lock_guard lock(mutex);
        stderr_tail.append(data, size);
        if (stderr_tail.size() > stderr_cap) {
            stderr_tail.erase(0, stderr_tail.size() - stderr_cap);
        }
    }
};

Process::Process()
    : _impl(std::make_unique<Impl>())
{
}

Process::~Process()
{
    // Dropping the last reference must not leave a child running or a
    // reader thread blocked on a live pipe; terminate is idempotent.
    terminate();
}

#ifndef _WIN32

namespace {

    // SIGPIPE is blocked for the duration of the write so an early child
    // exit surfaces as EPIPE instead of killing the process.
    class SigpipeBlock {
    public:
        SigpipeBlock()
        {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &mask, &_previous);
        }
        ~SigpipeBlock() { pthread_sigmask(SIG_SETMASK, &_previous, nullptr); }

    private:
        sigset_t _previous { };
    };

    bool write_all(int fd, const char* data, std::size_t size)
    {
        const SigpipeBlock blocked;
        std::size_t written = 0;
        while (written < size) {
            const ssize_t n = ::write(fd, data + written, size - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        return true;
    }

    void read_loop(int fd, Process::Impl& impl, bool is_stdout)
    {
        char buffer[4096];
        while (true) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                if (is_stdout) {
                    {
                        std::lock_guard lock(impl.mutex);
                        impl.stdout_buffer.append(
                            buffer, static_cast<std::size_t>(n));
                    }
                    impl.publish_lines(false);
                } else {
                    impl.append_stderr(buffer, static_cast<std::size_t>(n));
                }
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break; // EOF or error
        }
        if (is_stdout) {
            impl.mark_stdout_closed();
        }
    }

    // Waits up to `ms` for the child; true when it exited (and reaped).
    bool wait_for_exit(pid_t pid, long ms, int* status)
    {
        const long step = 20;
        for (long waited = 0; waited < ms; waited += step) {
            const pid_t r = ::waitpid(pid, status, WNOHANG);
            if (r == pid) {
                return true;
            }
            if (r < 0 && errno != EINTR) {
                return true; // already reaped or gone
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
        }
        const pid_t r = ::waitpid(pid, status, WNOHANG);
        return r == pid;
    }

} // namespace

std::unique_ptr<Process> Process::spawn(
    const ProcessOptions& options, std::string& error)
{
    if (options.argv.empty() || options.argv[0].empty()) {
        error = "empty command";
        return nullptr;
    }

    int in_pipe[2];
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return nullptr;
    }

    std::vector<char*> argv;
    argv.reserve(options.argv.size() + 1);
    for (const std::string& arg : options.argv) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    std::vector<char*> envp;
    if (!options.env.empty()) {
        for (char** entry = environ; *entry != nullptr; ++entry) {
            const std::string_view line(*entry);
            const std::size_t eq        = line.find('=');
            const std::string_view name = line.substr(0, eq);
            if (options.env.count(std::string(name)) == 0) {
                env_storage.emplace_back(line);
            }
        }
        for (const auto& [name, value] : options.env) {
            env_storage.push_back(name + "=" + value);
        }
        for (std::string& entry : env_storage) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[1]);
    if (!options.working_directory.empty()) {
        posix_spawn_file_actions_addchdir_np(
            &actions, options.working_directory.c_str());
    }

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Own process group so terminate() can signal the whole tree.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid         = -1;
    const int spawned = posix_spawnp(&pid, options.argv[0].c_str(), &actions,
        &attributes, argv.data(), options.env.empty() ? environ : envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);

    // The parent keeps only the ends it uses; the child's copies are
    // closed here so EOF is observed when the child exits.
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    if (spawned != 0) {
        error = std::string("spawn: ") + std::strerror(spawned);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        return nullptr;
    }

    std::unique_ptr<Process> process(new Process());
    Process::Impl& impl = *process->_impl;
    impl.pid            = pid;
    impl.stdin_fd       = in_pipe[1];
    impl.stdout_fd      = out_pipe[0];
    impl.stderr_fd      = err_pipe[0];
    impl.stderr_cap     = options.stderr_cap;
    impl.running.store(true);

    Process::Impl* raw = &impl;
    impl.stdout_reader
        = std::thread([raw] { read_loop(raw->stdout_fd, *raw, true); });
    impl.stderr_reader
        = std::thread([raw] { read_loop(raw->stderr_fd, *raw, false); });
    return process;
}

Status Process::write_line(std::string_view line)
{
    if (!_impl->running.load() || _impl->stdin_fd < 0) {
        return Status::NETWORK_ERROR;
    }
    std::string payload(line);
    payload += '\n';
    if (!write_all(_impl->stdin_fd, payload.data(), payload.size())) {
        return Status::NETWORK_ERROR;
    }
    return Status::OK;
}

Status Process::read_line(std::string& line, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(_impl->mutex);
    if (!_impl->cv.wait_for(lock, timeout,
            [this] { return !_impl->lines.empty() || _impl->stdout_closed; })) {
        return Status::TIMEOUT;
    }
    if (_impl->lines.empty()) {
        return Status::NETWORK_ERROR; // stream closed with no line
    }
    line = std::move(_impl->lines.front());
    _impl->lines.pop_front();
    return Status::OK;
}

bool Process::running() const { return _impl->running.load(); }

std::string Process::stderr_tail() const
{
    std::lock_guard lock(_impl->mutex);
    return _impl->stderr_tail;
}

void Process::terminate()
{
    Process::Impl& impl = *_impl;
    if (impl.reaped.exchange(true)) {
        return;
    }
    // Close stdin first: a cooperative server exits on EOF.
    if (impl.stdin_fd >= 0) {
        ::close(impl.stdin_fd);
        impl.stdin_fd = -1;
    }
    const long grace = 2000;
    int status       = 0;
    if (!wait_for_exit(impl.pid, grace, &status)) {
        killpg(impl.pid, SIGTERM);
        if (!wait_for_exit(impl.pid, grace, &status)) {
            killpg(impl.pid, SIGKILL);
            wait_for_exit(impl.pid, grace, &status);
        }
    }
    impl.running.store(false);
    // Unblock any reader waiting on the line queue.
    {
        std::lock_guard lock(impl.mutex);
        impl.stdout_closed = true;
    }
    impl.cv.notify_all();
    if (impl.stdout_reader.joinable()) {
        impl.stdout_reader.join();
    }
    if (impl.stderr_reader.joinable()) {
        impl.stderr_reader.join();
    }
}

#else // _WIN32

namespace {

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

    std::string to_utf8(const std::wstring& s)
    {
        if (s.empty()) {
            return "";
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(),
            static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
            out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::string quote_arg(const std::string& arg)
    {
        if (arg.find_first_of(" \t\"") == std::string::npos) {
            return arg;
        }
        std::string out = "\"";
        for (const char c : arg) {
            if (c == '"') {
                out += "\\\"";
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

    void read_loop(HANDLE handle, Process::Impl& impl, bool is_stdout)
    {
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(handle, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
            if (is_stdout) {
                {
                    std::lock_guard lock(impl.mutex);
                    impl.stdout_buffer.append(buffer, n);
                }
                impl.publish_lines(false);
            } else {
                impl.append_stderr(buffer, n);
            }
        }
        if (is_stdout) {
            impl.mark_stdout_closed();
        }
    }

} // namespace

std::unique_ptr<Process> Process::spawn(
    const ProcessOptions& options, std::string& error)
{
    if (options.argv.empty() || options.argv[0].empty()) {
        error = "empty command";
        return nullptr;
    }

    SECURITY_ATTRIBUTES sa { };
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_r = nullptr, in_w = nullptr;
    HANDLE out_r = nullptr, out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)
        || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        error = "CreatePipe failed";
        return nullptr;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    std::string command_line;
    for (const std::string& arg : options.argv) {
        if (!command_line.empty()) {
            command_line += ' ';
        }
        command_line += quote_arg(arg);
    }

    std::wstring environment;
    if (!options.env.empty()) {
        for (const auto& [name, value] : options.env) {
            environment += to_wide(name + "=" + value);
            environment.push_back(L'\0');
        }
        environment.push_back(L'\0');
    }

    STARTUPINFOW si { };
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = in_r;
    si.hStdOutput = out_w;
    si.hStdError  = err_w;

    PROCESS_INFORMATION pi { };
    const std::wstring wide_command   = to_wide(command_line);
    const std::wstring wide_directory = options.working_directory.empty()
        ? std::wstring { }
        : options.working_directory.wstring();
    const BOOL ok                     = CreateProcessW(nullptr,
        const_cast<wchar_t*>(wide_command.data()), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED, options.env.empty() ? nullptr : environment.data(),
        wide_directory.empty() ? nullptr : wide_directory.c_str(), &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        error = "CreateProcess failed";
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return nullptr;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info { };
        info.BasicLimitInformation.LimitFlags
            = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(
            job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::unique_ptr<Process> process(new Process());
    Process::Impl& impl = *process->_impl;
    impl.process        = pi.hProcess;
    impl.job            = job;
    impl.stdin_w        = in_w;
    impl.stdout_r       = out_r;
    impl.stderr_r       = err_r;
    impl.stderr_cap     = options.stderr_cap;
    impl.running.store(true);

    Process::Impl* raw = &impl;
    impl.stdout_reader
        = std::thread([raw] { read_loop(raw->stdout_r, *raw, true); });
    impl.stderr_reader
        = std::thread([raw] { read_loop(raw->stderr_r, *raw, false); });
    return process;
}

Status Process::write_line(std::string_view line)
{
    if (!_impl->running.load() || _impl->stdin_w == nullptr) {
        return Status::NETWORK_ERROR;
    }
    std::string payload(line);
    payload += '\n';
    DWORD written = 0;
    if (!WriteFile(_impl->stdin_w, payload.data(),
            static_cast<DWORD>(payload.size()), &written, nullptr)
        || written != payload.size()) {
        return Status::NETWORK_ERROR;
    }
    return Status::OK;
}

Status Process::read_line(std::string& line, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(_impl->mutex);
    if (!_impl->cv.wait_for(lock, timeout,
            [this] { return !_impl->lines.empty() || _impl->stdout_closed; })) {
        return Status::TIMEOUT;
    }
    if (_impl->lines.empty()) {
        return Status::NETWORK_ERROR;
    }
    line = std::move(_impl->lines.front());
    _impl->lines.pop_front();
    return Status::OK;
}

bool Process::running() const { return _impl->running.load(); }

std::string Process::stderr_tail() const
{
    std::lock_guard lock(_impl->mutex);
    return _impl->stderr_tail;
}

void Process::terminate()
{
    Process::Impl& impl = *_impl;
    if (impl.reaped.exchange(true)) {
        return;
    }
    if (impl.stdin_w != nullptr) {
        CloseHandle(impl.stdin_w);
        impl.stdin_w = nullptr;
    }
    if (impl.process != nullptr) {
        if (WaitForSingleObject(impl.process, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(impl.process, 1);
            WaitForSingleObject(impl.process, 2000);
        }
    }
    impl.running.store(false);
    {
        std::lock_guard lock(impl.mutex);
        impl.stdout_closed = true;
    }
    impl.cv.notify_all();
    if (impl.stdout_reader.joinable()) {
        impl.stdout_reader.join();
    }
    if (impl.stderr_reader.joinable()) {
        impl.stderr_reader.join();
    }
}

#endif

} // namespace imza
