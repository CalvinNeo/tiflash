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

/// In this header defines interfaces that exposed to KVStore layer.

#pragma once

#include <Storages/DeltaMerge/RowKeyRange.h>

namespace DB
{
namespace DM
{
struct RaftWriteResult
{
    // We will find all segments and regions by this range.
    std::vector<RowKeyRange> pending_flush_ranges;
    KeyspaceID keyspace_id;
    TableID table_id;

    RaftWriteResult(std::vector<RowKeyRange> && ranges, KeyspaceID keyspace, TableID table_id_)
        : pending_flush_ranges(std::move(ranges))
        , keyspace_id(keyspace)
        , table_id(table_id_)
    {}

    DISALLOW_COPY(RaftWriteResult);

    RaftWriteResult(RaftWriteResult && other)
    {
        pending_flush_ranges = std::move(other.pending_flush_ranges);
        keyspace_id = other.keyspace_id;
        table_id = other.table_id;
    }

    RaftWriteResult & operator=(RaftWriteResult && other)
    {
        pending_flush_ranges = std::move(other.pending_flush_ranges);
        keyspace_id = other.keyspace_id;
        table_id = other.table_id;
        return *this;
    }
};
typedef std::optional<RaftWriteResult> WriteResult;
static_assert(std::is_move_constructible_v<WriteResult>);
} // namespace DM
} // namespace DB
