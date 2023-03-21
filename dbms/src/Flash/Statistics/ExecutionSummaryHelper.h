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

#include <Flash/Coprocessor/ExecutionSummary.h>

namespace DB
{
class DAGContext;

void fillTiExecutionSummary(
    DAGContext & dag_context,
    tipb::ExecutorExecutionSummary * execution_summary,
    ExecutionSummary & current,
    const String & executor_id,
    bool force_fill_executor_id);
} // namespace DB
