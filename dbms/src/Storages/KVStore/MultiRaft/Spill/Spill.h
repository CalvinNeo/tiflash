// Copyright 2024 PingCAP, Inc.
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

#include <Storages/KVStore/MultiRaft/RegionData.h>
#include <Storages/KVStore/Types.h>
#include <unordered_map>

namespace DB {
struct SpillingMemtable {
    RegionDefaultCFData default_cf;
    // Should we include RegionLockCFData? See following discussion.
};

using SpillingMemtablePtr = std::shared_ptr<SpillingMemtable>;

struct SpillingTxn {

};

using SpillingTxnMap = std::unordered_map<Timestamp, SpillingTxn>;

// TODO Safety maybe shares region's lock?
struct SpillTxnCtx {
    bool isLargeTxn(const Timestamp & ts) const {
        return txns.contains(ts);
    }
    // start_ts -> SpillingTxn
    SpillingTxnMap txns;
};

using SpillTxnCtxPtr = std::shared_ptr<SpillingTxn>;

} // namespace DB