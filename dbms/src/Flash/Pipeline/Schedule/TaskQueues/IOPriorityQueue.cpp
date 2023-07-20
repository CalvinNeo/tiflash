// Copyright 2023 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Flash/Pipeline/Schedule/TaskQueues/IOPriorityQueue.h>
#include <Flash/Pipeline/Schedule/Tasks/TaskHelper.h>
#include <common/likely.h>

namespace DB
{
namespace
{
template <typename Queue>
bool popTask(Queue & queue, TaskPtr & task)
{
    if (!queue.empty())
    {
        task = std::move(queue.front());
        queue.pop_front();
        return true;
    }
    else
    {
        return false;
    }
}

void moveCancelledTasks(std::list<TaskPtr> & normal_queue, std::deque<TaskPtr> & cancel_queue, const String & query_id)
{
    assert(!query_id.empty());
    for (auto it = normal_queue.begin(); it != normal_queue.end();)
    {
        if (query_id == (*it)->getQueryId())
        {
            cancel_queue.push_back(std::move(*it));
            it = normal_queue.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
} // namespace

IOPriorityQueue::~IOPriorityQueue()
{
    RUNTIME_ASSERT(cancel_task_queue.empty(), logger, "all task should be taken before it is destructed");
    RUNTIME_ASSERT(io_in_task_queue.empty(), logger, "all task should be taken before it is destructed");
    RUNTIME_ASSERT(io_out_task_queue.empty(), logger, "all task should be taken before it is destructed");
}

bool IOPriorityQueue::take(TaskPtr & task)
{
    std::unique_lock lock(mu);
    while (true)
    {
        if (popTask(cancel_task_queue, task))
            return true;

        bool io_out_first = ratio_of_out_to_in * total_io_in_time_microsecond >= total_io_out_time_microsecond;
        auto & first_queue = io_out_first ? io_out_task_queue : io_in_task_queue;
        auto & next_queue = io_out_first ? io_in_task_queue : io_out_task_queue;
        if (popTask(first_queue, task))
            return true;
        if (popTask(next_queue, task))
            return true;
        if (unlikely(is_finished))
            return false;
        cv.wait(lock);
    }
}

void IOPriorityQueue::updateStatistics(const TaskPtr &, ExecTaskStatus exec_task_status, UInt64 inc_ns)
{
    switch (exec_task_status)
    {
    case ExecTaskStatus::IO_IN:
        total_io_in_time_microsecond += (inc_ns / 1000);
        break;
    case ExecTaskStatus::IO_OUT:
        total_io_out_time_microsecond += (inc_ns / 1000);
        break;
    default:; // ignore not io status.
    }
}

bool IOPriorityQueue::empty() const
{
    std::lock_guard lock(mu);
    return cancel_task_queue.empty() && io_out_task_queue.empty() && io_in_task_queue.empty();
}

void IOPriorityQueue::finish()
{
    {
        std::lock_guard lock(mu);
        is_finished = true;
    }
    cv.notify_all();
}

void IOPriorityQueue::submitTaskWithoutLock(TaskPtr && task)
{
    if unlikely (cancel_query_id_cache.contains(task->getQueryId()))
    {
        cancel_task_queue.push_back(std::move(task));
        return;
    }

    auto status = task->getStatus();
    switch (status)
    {
    case ExecTaskStatus::IO_IN:
        io_in_task_queue.push_back(std::move(task));
        break;
    case ExecTaskStatus::IO_OUT:
        io_out_task_queue.push_back(std::move(task));
        break;
    default:
        throw Exception(fmt::format("Unexpected status: {}, IOPriorityQueue only accepts tasks with IO status", magic_enum::enum_name(status)));
    }
}

void IOPriorityQueue::submit(TaskPtr && task)
{
    if unlikely (is_finished)
    {
        FINALIZE_TASK(task);
        return;
    }

    {
        std::lock_guard lock(mu);
        submitTaskWithoutLock(std::move(task));
    }
    cv.notify_one();
}

void IOPriorityQueue::submit(std::vector<TaskPtr> & tasks)
{
    if (tasks.empty())
        return;

    if unlikely (is_finished)
    {
        FINALIZE_TASKS(tasks);
        return;
    }

    std::lock_guard lock(mu);
    for (auto & task : tasks)
    {
        submitTaskWithoutLock(std::move(task));
        cv.notify_one();
    }
}

void IOPriorityQueue::cancel(const String & query_id)
{
    if unlikely (query_id.empty())
        return;

    std::lock_guard lock(mu);
    if (cancel_query_id_cache.add(query_id))
    {
        moveCancelledTasks(io_in_task_queue, cancel_task_queue, query_id);
        moveCancelledTasks(io_out_task_queue, cancel_task_queue, query_id);
        cv.notify_all();
    }
}
} // namespace DB
