#include <Common/FailPoint.h>
#include <Flash/Coprocessor/TablesRegionsInfo.h>
#include <Flash/CoprocessorHandler.h>
#include <Storages/Transaction/KVStore.h>

namespace DB
{
namespace FailPoints
{
extern const char force_no_local_region_for_mpp_task[];
} // namespace FailPoints

SingleTableRegions & TablesRegionsInfo::getOrCreateTableRegionInfoByTableID(Int64 table_id)
{
    if (is_single_table)
        return table_regions_info_map.begin()->second;
    if (table_regions_info_map.find(table_id) == table_regions_info_map.end())
    {
        table_regions_info_map[table_id] = SingleTableRegions();
    }
    return table_regions_info_map.find(table_id)->second;
}
const SingleTableRegions & TablesRegionsInfo::getTableRegionInfoByTableID(Int64 table_id) const
{
    if (is_single_table)
        return table_regions_info_map.begin()->second;
    if (table_regions_info_map.find(table_id) != table_regions_info_map.end())
        return table_regions_info_map.find(table_id)->second;
    throw TiFlashException(fmt::format("Can't find region info for table id: {}", table_id), Errors::Coprocessor::BadRequest);
}

static bool needRemoteRead(const RegionInfo & region_info, const TMTContext & tmt_context)
{
    fiu_do_on(FailPoints::force_no_local_region_for_mpp_task, { return true; });
    RegionPtr current_region = tmt_context.getKVStore()->getRegion(region_info.region_id);
    if (current_region == nullptr || current_region->peerState() != raft_serverpb::PeerState::Normal)
        return true;
    auto meta_snap = current_region->dumpRegionMetaSnapshot();
    return meta_snap.ver != region_info.region_version;
}

static void insertRegionInfoToTablesRegionInfo(const google::protobuf::RepeatedPtrField<coprocessor::RegionInfo> & regions, Int64 table_id, TablesRegionsInfo & tables_region_infos, std::unordered_set<RegionID> & local_region_id_set, const TMTContext & tmt_context)
{
    auto & table_region_info = tables_region_infos.getOrCreateTableRegionInfoByTableID(table_id);
    for (const auto & r : regions)
    {
        RegionInfo region_info(r.region_id(), r.region_epoch().version(), r.region_epoch().conf_ver(), CoprocessorHandler::GenCopKeyRange(r.ranges()), nullptr);
        if (region_info.key_ranges.empty())
        {
            throw TiFlashException(
                fmt::format("Income key ranges is empty for region: {}", region_info.region_id),
                Errors::Coprocessor::BadRequest);
        }
        /// TiFlash does not support regions with duplicated region id, so for regions with duplicated
        /// region id, only the first region will be treated as local region
        ///
        /// 1. Currently TiDB can't provide a consistent snapshot of the region cache and it may be updated during the
        ///    planning stage of a query. The planner may see multiple versions of one region (on one TiFlash node).
        /// 2. Two regions with same region id won't have overlapping key ranges.
        /// 3. TiFlash will pick the right version of region for local read and others for remote read.
        /// 4. The remote read will fetch the newest region info via key ranges. So it is possible to find the region
        ///    is served by the same node (but still read from remote).
        bool duplicated_region = local_region_id_set.count(region_info.region_id) > 0;

        if (duplicated_region || needRemoteRead(region_info, tmt_context))
            table_region_info.remote_regions.push_back(region_info);
        else
        {
            table_region_info.local_regions.insert(std::make_pair(region_info.region_id, region_info));
            local_region_id_set.emplace(region_info.region_id);
        }
    }
}

TablesRegionsInfo TablesRegionsInfo::create(
    const google::protobuf::RepeatedPtrField<coprocessor::RegionInfo> & regions,
    const google::protobuf::RepeatedPtrField<coprocessor::TableRegions> & table_regions,
    const TMTContext & tmt_context)
{
    assert(regions.empty() || table_regions.empty());
    TablesRegionsInfo tables_regions_info(!regions.empty());
    std::unordered_set<RegionID> local_region_id_set;
    if (!regions.empty())
        insertRegionInfoToTablesRegionInfo(regions, InvalidTableID, tables_regions_info, local_region_id_set, tmt_context);
    else
    {
        for (const auto & table_region : table_regions)
        {
            assert(table_region.physical_table_id() != InvalidTableID);
            insertRegionInfoToTablesRegionInfo(table_region.regions(), table_region.physical_table_id(), tables_regions_info, local_region_id_set, tmt_context);
        }
        assert(static_cast<UInt64>(table_regions.size()) == tables_regions_info.tableCount());
    }
    return tables_regions_info;
}

} // namespace DB
