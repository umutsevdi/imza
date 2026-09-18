#include "runtime/subagent_manager.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

namespace imza {

SubagentManager::~SubagentManager() { stop(); }

SubagentHandle SubagentManager::start(std::string prompt, std::string model,
    std::string variant, bool visible, SubagentRunFn run,
    SubagentCompleteFn complete, std::shared_ptr<Session> session)
{
    auto promise = std::make_shared<std::promise<SubagentResult>>();
    SubagentHandle handle { 0, promise->get_future().share() };
    SubagentTask started;
    {
        std::lock_guard lock(_mutex);
        handle.id = _next_id++;
        started = SubagentTask { handle.id, std::move(prompt), std::move(model),
            std::move(variant), visible, SubagentTask::State::RUNNING,
            Status::OK, { }, std::move(session) };
        _tasks.push_back(started);
    }
    _changed.publish(SubagentEvent { SubagentEvent::Kind::STARTED, started });
    {
        std::lock_guard lock(_mutex);
        _workers.emplace_back(handle.id,
            std::jthread(
                [this, id = handle.id, promise, run = std::move(run),
                    complete = std::move(complete)](std::stop_token stop) {
                    SubagentResult result;
                    try {
                        result = run(std::move(stop));
                    } catch (...) {
                        result = { Status::API_ERROR,
                            "subagent terminated unexpectedly" };
                    }
                    SubagentTask finished;
                    {
                        std::lock_guard lock(_mutex);
                        const auto task = std::find_if(_tasks.begin(),
                            _tasks.end(), [id](const SubagentTask& candidate) {
                                return candidate.id == id;
                            });
                        if (task != _tasks.end()) {
                            task->status = result.status;
                            task->output = result.output;
                            task->state  = result.status == Status::OK
                                ? SubagentTask::State::COMPLETED
                                : SubagentTask::State::FAILED;
                            finished     = *task;
                        }
                    }
                    _changed.publish(SubagentEvent { result.status == Status::OK
                            ? SubagentEvent::Kind::COMPLETED
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
    std::lock_guard lock(_mutex);
    const auto worker = std::ranges::find(
        _workers, id, &std::pair<std::size_t, std::jthread>::first);
    if (worker == _workers.end()) {
        return false;
    }
    worker->second.request_stop();
    return true;
}

void SubagentManager::prune_completed()
{
    std::vector<std::pair<std::size_t, std::jthread>> completed_workers;
    {
        std::lock_guard lock(_mutex);
        std::unordered_set<std::size_t> completed_ids;
        for (const SubagentTask& task : _tasks) {
            if (task.state != SubagentTask::State::RUNNING) {
                completed_ids.insert(task.id);
            }
        }
        std::erase_if(_tasks, [&completed_ids](const SubagentTask& task) {
            return completed_ids.contains(task.id);
        });
        for (auto it = _workers.begin(); it != _workers.end();) {
            if (!completed_ids.contains(it->first)) {
                ++it;
                continue;
            }
            completed_workers.push_back(std::move(*it));
            it = _workers.erase(it);
        }
    }
}

void SubagentManager::stop()
{
    std::vector<std::pair<std::size_t, std::jthread>> workers;
    {
        std::lock_guard lock(_mutex);
        workers.swap(_workers);
    }
    for (auto& worker : workers) {
        worker.second.request_stop();
    }
}

std::vector<SubagentTask> SubagentManager::tasks() const
{
    std::lock_guard lock(_mutex);
    return _tasks;
}

std::size_t SubagentManager::running_count(bool visible_only) const
{
    std::lock_guard lock(_mutex);
    return static_cast<std::size_t>(std::count_if(
        _tasks.begin(), _tasks.end(), [visible_only](const SubagentTask& task) {
            return task.state == SubagentTask::State::RUNNING
                && (!visible_only || task.visible);
        }));
}

Signal<const SubagentEvent&>::Subscription SubagentManager::subscribe(
    Signal<const SubagentEvent&>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

} // namespace imza
