#include "runtime/bg_process_manager.h"

#include "platform/command_runner.h"

#include <atomic>
#include <future>
#include <utility>

namespace imza {

BgProcessManager::~BgProcessManager() { stop(); }

void BgProcessManager::stop()
{
    mark_dead();
    std::vector<Worker> workers;
    {
        std::lock_guard lock(_mutex);
        workers.swap(_workers);
    }
    for (Worker& worker : workers) {
        worker.thread.request_stop();
    }
    // The jthread destructors join as `workers` goes out of scope, so stop()
    // returns only after every background process has been killed and reaped.
}

bool BgProcessManager::start(const std::string& command,
    std::chrono::seconds timeout, const std::filesystem::path& workspace)
{
    auto promise  = std::make_shared<std::promise<bool>>();
    auto future   = promise->get_future();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(_mutex);
        // Reap workers whose process already ended so the vector does not
        // grow without bound across many background commands.
        std::erase_if(_workers,
            [](const Worker& worker) { return worker.finished->load(); });
        _workers.push_back(Worker { finished,
            std::jthread([command, timeout, workspace, promise, finished](
                             std::stop_token stop) {
                const bool spawned = spawn_background_command(command, timeout,
                    workspace, stop, [promise] { promise->set_value(true); });
                if (!spawned) {
                    promise->set_value(false);
                }
                finished->store(true);
            }) });
    }
    future.wait();
    return future.get();
}

} // namespace imza
