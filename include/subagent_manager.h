#pragma once

#include <cstddef>
#include <future>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "types.h"
#include "ursa_signal.h"

namespace ursa {

struct SubagentResult {
    Status status = Status::OK;
    std::string output;
};

struct SubagentTask {
    enum class State { RUNNING, COMPLETED, FAILED };

    std::size_t id = 0;
    std::string prompt;
    std::string model;
    std::string variant;
    bool visible = true;
    State state  = State::RUNNING;
    Status status = Status::OK;
    std::string output;
};

struct SubagentEvent {
    enum class Kind { STARTED, COMPLETED, FAILED };
    Kind kind = Kind::STARTED;
    SubagentTask task;
};

struct SubagentHandle {
    std::size_t id = 0;
    std::shared_future<SubagentResult> completion;
};

using SubagentRunFn = std::function<SubagentResult(std::stop_token)>;
using SubagentCompleteFn = std::function<void(const SubagentResult&)>;

class SubagentManager {
public:
    SubagentManager() = default;
    ~SubagentManager();

    SubagentManager(const SubagentManager&) = delete;
    SubagentManager& operator=(const SubagentManager&) = delete;

    SubagentHandle start(std::string prompt, std::string model,
        std::string variant, bool visible, SubagentRunFn run,
        SubagentCompleteFn complete = { });
    std::vector<SubagentTask> tasks(bool visible_only = false) const;
    std::size_t running_count(bool visible_only = true) const;
    [[nodiscard]] Signal<const SubagentEvent&>::Subscription subscribe(
        Signal<const SubagentEvent&>::Callback callback);

private:
    mutable std::mutex mutex_;
    std::vector<SubagentTask> tasks_;
    std::vector<std::jthread> workers_;
    std::size_t next_id_ = 1;
    Signal<const SubagentEvent&> changed_;
};

} // namespace ursa
