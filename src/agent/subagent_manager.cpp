#include "subagent_manager.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace ursa {

SubagentManager::~SubagentManager()
{
    stop();
}

SubagentHandle SubagentManager::start(std::string prompt, std::string model,
    std::string variant, bool visible, SubagentRunFn run,
    SubagentCompleteFn complete, std::shared_ptr<Session> session)
{
    auto promise = std::make_shared<std::promise<SubagentResult>>();
    SubagentHandle handle { 0, promise->get_future().share() };
    SubagentTask started;
    {
        std::lock_guard lock(mutex_);
        handle.id = next_id_++;
        started = SubagentTask { handle.id, std::move(prompt),
            std::move(model), std::move(variant), visible,
            SubagentTask::State::RUNNING, Status::OK, { },
            std::move(session) };
        tasks_.push_back(started);
    }
    changed_.publish(SubagentEvent { SubagentEvent::Kind::STARTED, started });
    {
        std::lock_guard lock(mutex_);
        workers_.emplace_back(handle.id, std::jthread([this, id = handle.id, promise,
                                  run = std::move(run),
                                  complete = std::move(complete)](
                                  std::stop_token stop) {
            SubagentResult result;
            try {
                result = run(stop);
            } catch (...) {
                result = { Status::API_ERROR, "subagent terminated unexpectedly" };
            }
            SubagentTask finished;
            {
                std::lock_guard lock(mutex_);
                const auto task = std::find_if(tasks_.begin(), tasks_.end(),
                    [id](const SubagentTask& candidate) {
                        return candidate.id == id;
                    });
                if (task != tasks_.end()) {
                    task->status = result.status;
                    task->output = result.output;
                    task->state  = result.status == Status::OK
                        ? SubagentTask::State::COMPLETED
                        : SubagentTask::State::FAILED;
                    finished = *task;
                }
            }
            changed_.publish(SubagentEvent {
                result.status == Status::OK ? SubagentEvent::Kind::COMPLETED
                                            : SubagentEvent::Kind::FAILED,
                std::move(finished) });
            if (complete) {
                complete(result);
            }
            promise->set_value(result);
        }));
    }
    return handle;
}

bool SubagentManager::cancel(std::size_t id)
{
    std::lock_guard lock(mutex_);
    const auto worker = std::ranges::find(workers_, id,
        &std::pair<std::size_t, std::jthread>::first);
    if (worker == workers_.end()) return false;
    worker->second.request_stop();
    return true;
}

void SubagentManager::stop()
{
    std::vector<std::pair<std::size_t, std::jthread>> workers;
    {
        std::lock_guard lock(mutex_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        worker.second.request_stop();
    }
}

std::vector<SubagentTask> SubagentManager::tasks(bool visible_only) const
{
    std::lock_guard lock(mutex_);
    if (!visible_only) {
        return tasks_;
    }
    std::vector<SubagentTask> visible;
    std::copy_if(tasks_.begin(), tasks_.end(), std::back_inserter(visible),
        [](const SubagentTask& task) { return task.visible; });
    return visible;
}

std::size_t SubagentManager::running_count(bool visible_only) const
{
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(tasks_.begin(), tasks_.end(),
        [visible_only](const SubagentTask& task) {
            return task.state == SubagentTask::State::RUNNING
                && (!visible_only || task.visible);
        }));
}

Signal<const SubagentEvent&>::Subscription SubagentManager::subscribe(
    Signal<const SubagentEvent&>::Callback callback)
{
    return changed_.subscribe(std::move(callback));
}

} // namespace ursa
