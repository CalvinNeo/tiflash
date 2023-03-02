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

#pragma once

#include <Flash/Pipeline/Schedule/Events/PipelineEvent.h>

namespace DB
{
class PlainPipelineEvent : public PipelineEvent
{
public:
    PlainPipelineEvent(
        PipelineExecutorStatus & exec_status_,
        MemoryTrackerPtr mem_tracker_,
        const String & req_id,
        Context & context_,
        const PipelinePtr & pipeline_,
        size_t concurrency_)
        : PipelineEvent(exec_status_, std::move(mem_tracker_), req_id, context_, pipeline_)
        , concurrency(concurrency_)
    {}

protected:
    std::vector<TaskPtr> scheduleImpl() override;

private:
    size_t concurrency;
};
} // namespace DB
