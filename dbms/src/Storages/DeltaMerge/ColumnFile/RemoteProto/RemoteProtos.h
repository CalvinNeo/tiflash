// Copyright 2022 PingCAP, Ltd.
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

#include <Storages/DeltaMerge/ColumnFile/ColumnFile.h>
#include <Storages/DeltaMerge/RowKeyRange.h>
#include <Storages/Page/universal/Readers.h>

namespace DB
{
namespace DM
{
namespace RemoteProtocol
{

// TODO: These should be generated by protobuf.

struct ColumnFileInMemory
{
    String schema;
    std::vector<String> block_columns;

    UInt64 rows;
};

struct ColumnFileTiny
{
    String schema;
    UInt64 page_id;

    UInt64 rows;
    UInt64 bytes;
};

struct ColumnFileBig
{
    UInt64 file_id;
};

struct ColumnFileDeleteRange
{
    RowKeyRange range;
};

using ColumnFile = std::variant<ColumnFileInMemory, ColumnFileTiny, ColumnFileBig, ColumnFileDeleteRange>;

} // namespace RemoteProtocol
} // namespace DM
} // namespace DB
