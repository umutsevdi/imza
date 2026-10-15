#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/types.h"

namespace imza {

// Owns the reaper threads of detached background commands started through
// imza.bg.shell. Each thread blocks until its process exits, its timeout
// elapses, or stop() is requested (then the process group is killed).
// Destruction stops and joins every worker, so application teardown kills
// the background processes it started.
class BgProcessManager final : public ApplicationComponent {
public:
    BgProcessManager() = default;
    ~BgProcessManager();

    BgProcessManager(const BgProcessManager&)            = delete;
    BgProcessManager& operator=(const BgProcessManager&) = delete;

    // Spawns `command` detached (stdio on /dev/null; the command must
    // redirect its own output) and returns once the fork succeeds. False
    // when it could not be spawned.
    bool start(const std::string& command, std::chrono::seconds timeout,
        const std::filesystem::path& workspace);
    void stop();

private:
    struct Worker {
        std::shared_ptr<std::atomic<bool>> finished;
        std::jthread thread;
    };

    mutable std::mutex _mutex;
    std::vector<Worker> _workers;
};

} // namespace imza
