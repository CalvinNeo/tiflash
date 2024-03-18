// Copyright 2023 PingCAP, Inc.
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

#include <Storages/DeltaMerge/Index/MinMaxIndex.h>
#include <Storages/DeltaMerge/Index/VectorIndex_fwd.h>

namespace DB
{
namespace DM
{
class EqualIndex;
using EqualIndexPtr = std::shared_ptr<EqualIndex>;


class EqualIndex
{
public:
    virtual ~EqualIndex() = default;
};

struct RSIndex
{
    DataTypePtr type;
    MinMaxIndexPtr minmax;
    EqualIndexPtr equal;
    VectorIndexPtr vector; // TODO: Actually this is not a rough index. We put it here for convenience.

    RSIndex(const DataTypePtr & type_, const MinMaxIndexPtr & minmax_)
        : type(type_)
        , minmax(minmax_)
    {}

    RSIndex(const DataTypePtr & type_, const VectorIndexPtr & vector_)
        : type(type_)
        , vector(vector_)
    {}

    RSIndex(const DataTypePtr & type_, const MinMaxIndexPtr & minmax_, const EqualIndexPtr & equal_)
        : type(type_)
        , minmax(minmax_)
        , equal(equal_)
    {}
};

using ColumnIndexes = std::unordered_map<ColId, RSIndex>;

} // namespace DM

} // namespace DB
