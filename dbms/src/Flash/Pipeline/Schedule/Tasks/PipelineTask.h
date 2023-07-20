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

#include <Flash/Pipeline/Exec/PipelineExec.h>
#include <Flash/Pipeline/Schedule/Tasks/EventTask.h>

namespace DB
{
class PipelineTask : public EventTask
{
public:
    PipelineTask(
        PipelineExecutorContext & exec_context_,
        const String & req_id,
        const EventPtr & event_,
        PipelineExecPtr && pipeline_exec_);

protected:
    ExecTaskStatus executeImpl() override;

    ExecTaskStatus executeIOImpl() override;

    ExecTaskStatus awaitImpl() override;

    void doFinalizeImpl() override;

private:
    PipelineExecPtr pipeline_exec_holder;
    // To reduce the overheads of `pipeline_exec_holder.get()`
    PipelineExec * pipeline_exec;
};
} // namespace DB
