#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace imza {

// Options for spawning one child process with piped stdin/stdout/stderr.
// argv is passed verbatim (no shell): argv[0] is the program, looked up on
// PATH. env entries are merged over the inherited environment.
struct ProcessOptions {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    std::filesystem::path working_directory;
    // Time to wait for a natural exit after stdin closes, and again after
    // SIGTERM, before escalating to SIGKILL.
    std::chrono::milliseconds terminate_grace { 2000 };
    std::size_t stderr_cap = 8192;
};

// A spawned child with line-oriented pipes. stdout is read as newline-
// delimited lines; stderr is drained into a bounded tail so a chatty child
// cannot block on a full pipe. Thread-safe: write_line, read_line, and
// terminate may be called from different threads.
class Process {
public:
    static std::unique_ptr<Process> spawn(
        const ProcessOptions& options, std::string& error);
    ~Process();

    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    // Writes one newline-terminated line to the child's stdin.
    Status write_line(std::string_view line);
    // Reads one newline-delimited stdout line. Status::TIMEOUT when none
    // arrives in time; Status::NETWORK_ERROR when the stream closed.
    Status read_line(std::string& line, std::chrono::milliseconds timeout);
    bool running() const;
    // Retained stderr tail (last stderr_cap bytes); diagnostics only.
    std::string stderr_tail() const;
    // Idempotent: close stdin, wait grace, SIGTERM, wait, SIGKILL, reap.
    void terminate();

    // Opaque state, public so the free helpers in the translation unit can
    // name it; the definition never leaves src/platform/process.cpp.
    struct Impl;

private:
    Process();
    std::unique_ptr<Impl> _impl;
};

} // namespace imza
