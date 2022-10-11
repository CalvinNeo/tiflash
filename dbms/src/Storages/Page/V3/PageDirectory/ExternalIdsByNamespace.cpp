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

#include <Storages/Page/PageDefines.h>
#include <Storages/Page/V3/PageDirectory/ExternalIdsByNamespace.h>

#include <mutex>

namespace DB::PS::V3
{
template <typename Trait>
void ExternalIdsByNamespace<Trait>::addExternalIdUnlock(const std::shared_ptr<PageId> & external_id)
{
    const Prefix & ns_id = Trait::getPrefix(*external_id);
    // create a new ExternalIds if the ns_id is not exists, else return
    // the existing one.
    auto [ns_iter, new_inserted] = ids_by_ns.try_emplace(ns_id, ExternalIds{});
    ns_iter->second.emplace_back(std::weak_ptr<PageId>(external_id));
}

template <typename Trait>
void ExternalIdsByNamespace<Trait>::addExternalId(const std::shared_ptr<PageId> & external_id)
{
    std::unique_lock map_guard(mu);
    addExternalIdUnlock(external_id);
}

template <typename Trait>
std::set<DB::PageId>
ExternalIdsByNamespace<Trait>::getAliveIds(const Prefix & ns_id) const
{
    // Now we assume a lock among all prefixes is good enough.
    std::unique_lock map_guard(mu);

    std::set<DB::PageId> valid_external_ids;
    auto ns_iter = ids_by_ns.find(ns_id);
    if (ns_iter == ids_by_ns.end())
        return valid_external_ids;

    // Only scan the given `ns_id`
    auto & external_ids = ns_iter->second;
    for (auto iter = external_ids.begin(); iter != external_ids.end(); /*empty*/)
    {
        if (auto holder = iter->lock(); holder == nullptr)
        {
            // the external id has been removed from `PageDirectory`,
            // cleanup the invalid weak_ptr
            iter = external_ids.erase(iter);
            continue;
        }
        else
        {
            valid_external_ids.emplace(Trait::getU64ID(*holder));
            ++iter;
        }
    }
    // No valid external pages in this `ns_id`
    if (valid_external_ids.empty())
    {
        ids_by_ns.erase(ns_id);
    }
    return valid_external_ids;
}

template <typename Trait>
void ExternalIdsByNamespace<Trait>::unregisterNamespace(const Prefix & ns_id)
{
    std::unique_lock map_guard(mu);
    // free all weak_ptrs of this namespace
    ids_by_ns.erase(ns_id);
}

template class ExternalIdsByNamespace<u128::ExternalIdTrait>;
template class ExternalIdsByNamespace<universal::ExternalIdTrait>;

} // namespace DB::PS::V3
