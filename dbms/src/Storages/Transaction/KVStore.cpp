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

#include <Common/FmtUtils.h>
#include <Common/Stopwatch.h>
#include <Common/SyncPoint/SyncPoint.h>
#include <Common/TiFlashMetrics.h>
#include <Common/setThreadName.h>
#include <Interpreters/Context.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/StorageDeltaMerge.h>
#include <Storages/StorageDeltaMergeHelpers.h>
#include <Storages/Transaction/BackgroundService.h>
#include <Storages/Transaction/KVStore.h>
#include <Storages/Transaction/ProxyFFI.h>
#include <Storages/Transaction/ReadIndexWorker.h>
#include <Storages/Transaction/Region.h>
#include <Storages/Transaction/RegionExecutionResult.h>
#include <Storages/Transaction/RegionPersister.h>
#include <Storages/Transaction/RegionTable.h>
#include <Storages/Transaction/TMTContext.h>
#include <common/likely.h>

#include <tuple>
#include <variant>

namespace DB
{
namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int TABLE_IS_DROPPED;
} // namespace ErrorCodes

namespace FailPoints
{
extern const char force_fail_in_flush_region_data[];
extern const char pause_proactive_flush_before_persist_region[];
extern const char pause_passive_flush_before_persist_region[];
extern const char proactive_flush_between_persist_cache_and_region[];
extern const char proactive_flush_between_persist_regions[];
} // namespace FailPoints

KVStore::KVStore(Context & context)
    : region_persister(context.getSharedContextDisagg()->isDisaggregatedComputeMode() ? nullptr : std::make_unique<RegionPersister>(context, region_manager))
    , raft_cmd_res(std::make_unique<RaftCommandResult>())
    , log(Logger::get())
    , region_compact_log_period(120)
    , region_compact_log_min_rows(40 * 1024)
    , region_compact_log_min_bytes(32 * 1024 * 1024)
    , region_compact_log_gap(500)
{
    // default config about compact-log: period 120s, rows 40k, bytes 32MB.
    LOG_INFO(log, "KVStore inited");
}

void KVStore::restore(PathPool & path_pool, const TiFlashRaftProxyHelper * proxy_helper)
{
    if (!region_persister)
        return;

    auto task_lock = genTaskLock();
    auto manage_lock = genRegionMgrWriteLock(task_lock);

    this->proxy_helper = proxy_helper;
    manage_lock.regions = region_persister->restore(path_pool, proxy_helper);

    LOG_INFO(log, "Restored {} regions", manage_lock.regions.size());

    // init range index
    for (const auto & [id, region] : manage_lock.regions)
    {
        std::ignore = id;
        manage_lock.index.add(region);
    }

    {
        const size_t batch = 512;
        std::vector<std::stringstream> msgs;
        msgs.resize(batch);

        // init range index
        for (const auto & [id, region] : manage_lock.regions)
        {
            msgs[id % batch] << region->getDebugString() << ";";
        }

        for (const auto & msg : msgs)
        {
            auto str = msg.str();
            if (!str.empty())
                LOG_INFO(log, "{}", str);
        }
    }
}

RegionPtr KVStore::getRegion(RegionID region_id) const
{
    auto manage_lock = genRegionMgrReadLock();
    if (auto it = manage_lock.regions.find(region_id); it != manage_lock.regions.end())
        return it->second;
    return nullptr;
}
// TODO: may get regions not in segment?
RegionMap KVStore::getRegionsByRangeOverlap(const RegionRange & range) const
{
    auto manage_lock = genRegionMgrReadLock();
    return manage_lock.index.findByRangeOverlap(range);
}

RegionTaskLock RegionTaskCtrl::genRegionTaskLock(RegionID region_id) const
{
    RegionTaskElement * e = nullptr;
    {
        auto _ = genLockGuard();
        auto it = regions.try_emplace(region_id).first;
        e = &it->second;
    }
    return RegionTaskLock(e->mutex);
}

RegionTaskLock RegionManager::genRegionTaskLock(RegionID region_id) const
{
    return region_task_ctrl.genRegionTaskLock(region_id);
}

size_t KVStore::regionSize() const
{
    auto manage_lock = genRegionMgrReadLock();
    return manage_lock.regions.size();
}

void KVStore::traverseRegions(std::function<void(RegionID, const RegionPtr &)> && callback) const
{
    auto manage_lock = genRegionMgrReadLock();
    for (const auto & region : manage_lock.regions)
        callback(region.first, region.second);
}

bool KVStore::tryFlushRegionCacheInStorage(TMTContext & tmt, const Region & region, const LoggerPtr & log, bool try_until_succeed)
{
    fiu_do_on(FailPoints::force_fail_in_flush_region_data, { return false; });
    auto keyspace_id = region.getKeyspaceID();
    auto table_id = region.getMappedTableID();
    auto storage = tmt.getStorages().get(keyspace_id, table_id);
    if (unlikely(storage == nullptr))
    {
        LOG_WARNING(log,
                    "tryFlushRegionCacheInStorage can not get table for region {} with table id {}, ignored",
                    region.toString(),
                    table_id);
        return true;
    }

    try
    {
        // Acquire `drop_lock` so that no other threads can drop the storage during `flushCache`. `alter_lock` is not required.
        auto storage_lock = storage->lockForShare(getThreadNameAndID());
        auto rowkey_range = DM::RowKeyRange::fromRegionRange(
            region.getRange(),
            region.getRange()->getMappedTableID(),
            storage->isCommonHandle(),
            storage->getRowKeyColumnSize());
        return storage->flushCache(tmt.getContext(), rowkey_range, try_until_succeed);
    }
    catch (DB::Exception & e)
    {
        // We can ignore if storage is already dropped.
        if (e.code() != ErrorCodes::TABLE_IS_DROPPED)
            throw;
    }
    return true;
}

void KVStore::tryPersistRegion(RegionID region_id)
{
    auto region = getRegion(region_id);
    if (region)
    {
        persistRegion(*region, std::nullopt, PersistRegionReason::Debug, "");
    }
}

void KVStore::gcRegionPersistedCache(Seconds gc_persist_period)
{
    {
        decltype(bg_gc_region_data) tmp;
        std::lock_guard lock(bg_gc_region_data_mutex);
        tmp.swap(bg_gc_region_data);
    }
    Timepoint now = Clock::now();
    if (now < (last_gc_time.load() + gc_persist_period))
        return;
    last_gc_time = now;
    RUNTIME_CHECK_MSG(region_persister, "try access to region_persister without initialization, stack={}", StackTrace().toString());
    region_persister->gc();
}

void KVStore::removeRegion(RegionID region_id, bool remove_data, RegionTable & region_table, const KVStoreTaskLock & task_lock, const RegionTaskLock & region_lock)
{
    LOG_INFO(log, "Start to remove [region {}]", region_id);

    {
        auto manage_lock = genRegionMgrWriteLock(task_lock);
        auto it = manage_lock.regions.find(region_id);
        manage_lock.index.remove(it->second->makeRaftCommandDelegate(task_lock).getRange().comparableKeys(), region_id); // remove index
        manage_lock.regions.erase(it);
    }
    {
        if (read_index_worker_manager) //std::atomic_thread_fence will protect it
        {
            // remove cache & read-index task
            read_index_worker_manager->getWorkerByRegion(region_id).removeRegion(region_id);
        }
    }

    RUNTIME_CHECK_MSG(region_persister, "try access to region_persister without initialization, stack={}", StackTrace().toString());
    region_persister->drop(region_id, region_lock);
    LOG_INFO(log, "Persisted [region {}] deleted", region_id);

    region_table.removeRegion(region_id, remove_data, region_lock);

    LOG_INFO(log, "Remove [region {}] done", region_id);
}

KVStoreTaskLock KVStore::genTaskLock() const
{
    return KVStoreTaskLock(task_mutex);
}

RegionManager::RegionReadLock KVStore::genRegionMgrReadLock() const
{
    return region_manager.genReadLock();
}

RegionManager::RegionWriteLock KVStore::genRegionMgrWriteLock(const KVStoreTaskLock &)
{
    return region_manager.genWriteLock();
}

EngineStoreApplyRes KVStore::handleWriteRaftCmdInner(
    raft_cmdpb::RaftCmdRequest && request,
    UInt64 region_id,
    UInt64 index,
    UInt64 term,
    TMTContext & tmt,
    DM::WriteResult & write_result)
{
    std::vector<BaseBuffView> keys;
    std::vector<BaseBuffView> vals;
    std::vector<WriteCmdType> cmd_types;
    std::vector<ColumnFamilyType> cmd_cf;
    keys.reserve(request.requests_size());
    vals.reserve(request.requests_size());
    cmd_types.reserve(request.requests_size());
    cmd_cf.reserve(request.requests_size());

    for (const auto & req : request.requests())
    {
        auto type = req.cmd_type();

        switch (type)
        {
        case raft_cmdpb::CmdType::Put:
            keys.push_back({req.put().key().data(), req.put().key().size()});
            vals.push_back({req.put().value().data(), req.put().value().size()});
            cmd_types.push_back(WriteCmdType::Put);
            cmd_cf.push_back(NameToCF(req.put().cf()));
            break;
        case raft_cmdpb::CmdType::Delete:
            keys.push_back({req.delete_().key().data(), req.delete_().key().size()});
            vals.push_back({nullptr, 0});
            cmd_types.push_back(WriteCmdType::Del);
            cmd_cf.push_back(NameToCF(req.delete_().cf()));
            break;
        default:
            throw Exception(fmt::format("Unsupport raft cmd {}", raft_cmdpb::CmdType_Name(type)), ErrorCodes::LOGICAL_ERROR);
        }
    }
    return handleWriteRaftCmdInner(
        WriteCmdsView{.keys = keys.data(), .vals = vals.data(), .cmd_types = cmd_types.data(), .cmd_cf = cmd_cf.data(), .len = keys.size()},
        region_id,
        index,
        term,
        tmt,
        write_result);
}

EngineStoreApplyRes KVStore::handleWriteRaftCmdInner(const WriteCmdsView & cmds, UInt64 region_id, UInt64 index, UInt64 term, TMTContext & tmt, DM::WriteResult & write_result)
{
    EngineStoreApplyRes res;
    {
        auto region_persist_lock = region_manager.genRegionTaskLock(region_id);

        const RegionPtr region = getRegion(region_id);
        if (region == nullptr)
        {
            return EngineStoreApplyRes::NotFound;
        }

        auto && [r, w] = region->handleWriteRaftCmd(cmds, index, term, tmt);

        if (region->getClusterRaftstoreVer() == RaftstoreVer::V2)
        {
            region->orphanKeysInfo().advanceAppliedIndex(index);
        }
        write_result = std::move(w);
        res = r;
    }
    /// Safety:
    /// This call is from Proxy's applying thread of this region, so:
    /// 1. No other thread can write from raft to this region even if we unlocked here.
    /// 2. If `proactiveFlushCacheAndRegion` causes a write stall, it will be forwarded to raft layer.
    if (write_result)
    {
        auto & inner = write_result.value();
        for (auto it = inner.pending_flush_ranges.begin(); it != inner.pending_flush_ranges.end(); it++)
        {
            proactiveFlushCacheAndRegion(tmt, *it, inner.keyspace_id, inner.table_id, false);
        }
    }
    return res;
}

EngineStoreApplyRes KVStore::handleWriteRaftCmd(
    raft_cmdpb::RaftCmdRequest && request,
    UInt64 region_id,
    UInt64 index,
    UInt64 term,
    TMTContext & tmt)
{
    DM::WriteResult write_result;
    return handleWriteRaftCmdInner(std::move(request), region_id, index, term, tmt, write_result);
}

EngineStoreApplyRes KVStore::handleWriteRaftCmd(const WriteCmdsView & cmds, UInt64 region_id, UInt64 index, UInt64 term, TMTContext & tmt)
{
    DM::WriteResult write_result;
    return handleWriteRaftCmdInner(cmds, region_id, index, term, tmt, write_result);
}

EngineStoreApplyRes KVStore::handleWriteRaftCmdDebug(raft_cmdpb::RaftCmdRequest && request, UInt64 region_id, UInt64 index, UInt64 term, TMTContext & tmt, DM::WriteResult & write_result)
{
    return handleWriteRaftCmdInner(std::move(request), region_id, index, term, tmt, write_result);
}

void KVStore::handleDestroy(UInt64 region_id, TMTContext & tmt)
{
    handleDestroy(region_id, tmt, genTaskLock());
}

void KVStore::handleDestroy(UInt64 region_id, TMTContext & tmt, const KVStoreTaskLock & task_lock)
{
    const auto region = getRegion(region_id);
    if (region == nullptr)
    {
        LOG_INFO(log, "[region {}] is not found, might be removed already", region_id);
        return;
    }
    LOG_INFO(log, "Handle destroy {}", region->toString());
    region->setPendingRemove();
    removeRegion(region_id, /* remove_data */ true, tmt.getRegionTable(), task_lock, region_manager.genRegionTaskLock(region_id));
}

void KVStore::setRegionCompactLogConfig(UInt64 sec, UInt64 rows, UInt64 bytes, UInt64 gap)
{
    region_compact_log_period = sec;
    region_compact_log_min_rows = rows;
    region_compact_log_min_bytes = bytes;
    region_compact_log_gap = gap;

    LOG_INFO(
        log,
        "threshold config: period {}, rows {}, bytes {}, gap {}",
        sec,
        rows,
        bytes,
        gap);
}

void KVStore::persistRegion(const Region & region, std::optional<const RegionTaskLock *> region_task_lock, PersistRegionReason reason, const char * extra_msg)
{
    RUNTIME_CHECK_MSG(region_persister, "try access to region_persister without initialization, stack={}", StackTrace().toString());
    if (region_task_lock.has_value())
    {
        auto reason_id = magic_enum::enum_underlying(reason);
        std::string caller = fmt::format("{} {}", PersistRegionReasonMap[reason_id], extra_msg);
        LOG_INFO(log, "Start to persist {}, cache size: {} bytes for `{}`", region.toString(true), region.dataSize(), caller);
        region_persister->persist(region, *region_task_lock.value());
        LOG_DEBUG(log, "Persist {} done", region.toString(false));
    }
    else
    {
        LOG_INFO(log, "Try to persist {}", region.toString(false));
        region_persister->persist(region);
        LOG_INFO(log, "After persisted {}, cache {} bytes", region.toString(false), region.dataSize());
    }
    switch (reason)
    {
    case PersistRegionReason::UselessAdminCommand:
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_useless_admin).Increment(1);
        break;
    case PersistRegionReason::AdminCommand:
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_useful_admin).Increment(1);
        break;
    case PersistRegionReason::Flush:
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_passive).Increment(1);
        break;
    case PersistRegionReason::ProactiveFlush:
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_proactive).Increment(1);
        break;
    case PersistRegionReason::ApplySnapshotPrevRegion:
    case PersistRegionReason::ApplySnapshotCurRegion:
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_apply_snapshot).Increment(1);
        break;
    case PersistRegionReason::IngestSst:
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_ingest_sst).Increment(1);
        break;
    default:
        break;
    }
}

bool KVStore::needFlushRegionData(UInt64 region_id, TMTContext & tmt)
{
    auto region_task_lock = region_manager.genRegionTaskLock(region_id);
    const RegionPtr curr_region_ptr = getRegion(region_id);
    // TODO Should handle when curr_region_ptr is null.
    return canFlushRegionDataImpl(curr_region_ptr, false, false, tmt, region_task_lock, 0, 0, 0, 0);
}

bool KVStore::tryFlushRegionData(UInt64 region_id, bool force_persist, bool try_until_succeed, TMTContext & tmt, UInt64 index, UInt64 term, uint64_t truncated_index, uint64_t truncated_term)
{
    auto region_task_lock = region_manager.genRegionTaskLock(region_id);
    const RegionPtr curr_region_ptr = getRegion(region_id);

    if (curr_region_ptr == nullptr)
    {
        /// If we can't find region here, we return true so proxy can trigger a CompactLog.
        /// The triggered CompactLog will be handled by `handleUselessAdminRaftCmd`,
        /// and result in a `EngineStoreApplyRes::NotFound`.
        /// Proxy will print this message and continue: `region not found in engine-store, maybe have exec `RemoveNode` first`.
        LOG_WARNING(log, "region {} [index: {}, term {}], not exist when flushing, maybe have exec `RemoveNode` first", region_id, index, term);
        return true;
    }

    FAIL_POINT_PAUSE(FailPoints::pause_passive_flush_before_persist_region);
    if (force_persist)
    {
        auto & curr_region = *curr_region_ptr;
        LOG_DEBUG(log, "{} flush region due to tryFlushRegionData by force, index {} term {}", curr_region.toString(false), index, term);
        if (!forceFlushRegionDataImpl(curr_region, try_until_succeed, tmt, region_task_lock, index, term))
        {
            throw Exception("Force flush region " + std::to_string(region_id) + " failed", ErrorCodes::LOGICAL_ERROR);
        }
        return true;
    }
    else
    {
        return canFlushRegionDataImpl(curr_region_ptr, true, try_until_succeed, tmt, region_task_lock, index, term, truncated_index, truncated_term);
    }
}

bool KVStore::canFlushRegionDataImpl(const RegionPtr & curr_region_ptr, UInt8 flush_if_possible, bool try_until_succeed, TMTContext & tmt, const RegionTaskLock & region_task_lock, UInt64 index, UInt64 term, UInt64 truncated_index, UInt64 truncated_term)
{
    if (curr_region_ptr == nullptr)
    {
        throw Exception("region not found when trying flush", ErrorCodes::LOGICAL_ERROR);
    }
    auto & curr_region = *curr_region_ptr;

    bool can_flush = false;
    auto [rows, size_bytes] = curr_region.getApproxMemCacheInfo();

    if (rows >= region_compact_log_min_rows.load(std::memory_order_relaxed))
    {
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_rowcount).Increment(1);
        can_flush = true;
    }
    if (size_bytes >= region_compact_log_min_bytes.load(std::memory_order_relaxed))
    {
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_size).Increment(1);
        can_flush = true;
    }
    auto gap_threshold = region_compact_log_gap.load();
    if (index > truncated_index + gap_threshold)
    {
        GET_METRIC(tiflash_raft_raft_events_count, type_flush_log_gap).Increment(1);
        can_flush = true;
    }

    auto current_gap = index > truncated_index ? index - truncated_index : 0;
    auto last_compact_log_applied = curr_region.lastCompactLogApplied();
    auto current_applied_gap = index > last_compact_log_applied ? index - last_compact_log_applied : 0;

    GET_METRIC(tiflash_raft_raft_events_count, type_pre_exec_compact).Increment(1);
    GET_METRIC(tiflash_raft_raft_log_lag_count, type_compact_index).Observe(current_gap);
    LOG_DEBUG(log, "{} approx mem cache info: rows {}, bytes {}, gap {}/{} applied gap {}", curr_region.toString(false), rows, size_bytes, current_gap, gap_threshold, current_applied_gap);

    if (can_flush && flush_if_possible)
    {
        GET_METRIC(tiflash_raft_raft_events_count, type_exec_compact).Increment(1);
        // This rarely happens when there are too may raft logs, which don't trigger a proactive flush.
        LOG_INFO(log, "{} flush region due to tryFlushRegionData, index {} term {} truncated_index {} truncated_term {} gap {}/{}", curr_region.toString(false), index, term, truncated_index, truncated_term, current_gap, gap_threshold);
        GET_METRIC(tiflash_raft_region_flush_size, type_flushed).Observe(size_bytes);
        return forceFlushRegionDataImpl(curr_region, try_until_succeed, tmt, region_task_lock, index, term);
    }
    else
    {
        GET_METRIC(tiflash_raft_region_flush_size, type_unflushed).Observe(size_bytes);
    }
    return can_flush;
}

bool KVStore::forceFlushRegionDataImpl(Region & curr_region, bool try_until_succeed, TMTContext & tmt, const RegionTaskLock & region_task_lock, UInt64 index, UInt64 term)
{
    Stopwatch watch;
    if (index)
    {
        // We set actual index when handling CompactLog.
        curr_region.handleWriteRaftCmd({}, index, term, tmt);
    }
    if (tryFlushRegionCacheInStorage(tmt, curr_region, log, try_until_succeed))
    {
        persistRegion(curr_region, &region_task_lock, PersistRegionReason::Flush, "");
        curr_region.markCompactLog();
        curr_region.cleanApproxMemCacheInfo();
        GET_METRIC(tiflash_raft_apply_write_command_duration_seconds, type_flush_region).Observe(watch.elapsedSeconds());
        return true;
    }
    else
    {
        return false;
    }
}

EngineStoreApplyRes KVStore::handleUselessAdminRaftCmd(
    raft_cmdpb::AdminCmdType cmd_type,
    UInt64 curr_region_id,
    UInt64 index,
    UInt64 term,
    TMTContext & tmt)
{
    auto region_task_lock = region_manager.genRegionTaskLock(curr_region_id);
    const RegionPtr curr_region_ptr = getRegion(curr_region_id);
    if (curr_region_ptr == nullptr)
    {
        return EngineStoreApplyRes::NotFound;
    }

    auto & curr_region = *curr_region_ptr;

    LOG_DEBUG(log,
              "{} handle ignorable admin command {} at [term: {}, index: {}]",
              curr_region.toString(false),
              raft_cmdpb::AdminCmdType_Name(cmd_type),
              term,
              index);

    if (curr_region.getClusterRaftstoreVer() == RaftstoreVer::V2)
    {
        curr_region.orphanKeysInfo().advanceAppliedIndex(index);
    }

    if (cmd_type == raft_cmdpb::AdminCmdType::CompactLog)
    {
        // Before CompactLog, we ought to make sure all data of this region are persisted.
        // So proxy will firstly call an FFI `fn_try_flush_data` to trigger a attempt to flush data on TiFlash's side.
        // The advance of apply index aka `handleWriteRaftCmd` is executed in `fn_try_flush_data`.
        // If the attempt fails, Proxy will filter execution of this CompactLog, which means every CompactLog observed by TiFlash can ALWAYS succeed now.
        // ref. https://github.com/pingcap/tidb-engine-ext/blob/e83a37d2d8d8ae1778fe279c5f06a851f8c9e56a/components/raftstore/src/engine_store_ffi/observer.rs#L175
        return EngineStoreApplyRes::Persist;
    }

    curr_region.handleWriteRaftCmd({}, index, term, tmt);
    if (cmd_type == raft_cmdpb::AdminCmdType::PrepareFlashback
        || cmd_type == raft_cmdpb::AdminCmdType::FinishFlashback
        || cmd_type == raft_cmdpb::AdminCmdType::BatchSwitchWitness)
    {
        tryFlushRegionCacheInStorage(tmt, curr_region, log);
        persistRegion(curr_region, &region_task_lock, PersistRegionReason::UselessAdminCommand, fmt::format("{}", cmd_type).c_str());
        return EngineStoreApplyRes::Persist;
    }
    return EngineStoreApplyRes::None;
}

EngineStoreApplyRes KVStore::handleAdminRaftCmd(raft_cmdpb::AdminRequest && request,
                                                raft_cmdpb::AdminResponse && response,
                                                UInt64 curr_region_id,
                                                UInt64 index,
                                                UInt64 term,
                                                TMTContext & tmt)
{
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_raft_apply_write_command_duration_seconds, type_admin).Observe(watch.elapsedSeconds());
    });
    auto type = request.cmd_type();
    switch (request.cmd_type())
    {
    // CompactLog | VerifyHash | ComputeHash won't change region meta, there is no need to occupy task lock of kvstore.
    case raft_cmdpb::AdminCmdType::CompactLog:
    case raft_cmdpb::AdminCmdType::VerifyHash:
    case raft_cmdpb::AdminCmdType::ComputeHash:
    case raft_cmdpb::AdminCmdType::PrepareFlashback:
    case raft_cmdpb::AdminCmdType::FinishFlashback:
    case raft_cmdpb::AdminCmdType::BatchSwitchWitness:
        return handleUselessAdminRaftCmd(type, curr_region_id, index, term, tmt);
    default:
        break;
    }

    RegionTable & region_table = tmt.getRegionTable();

    // Lock the whole kvstore.
    auto task_lock = genTaskLock();

    {
        auto region_task_lock = region_manager.genRegionTaskLock(curr_region_id);
        const RegionPtr curr_region_ptr = getRegion(curr_region_id);
        if (curr_region_ptr == nullptr)
        {
            LOG_WARNING(log,
                        "[region {}] is not found at [term {}, index {}, cmd {}], might be removed already",
                        curr_region_id,
                        term,
                        index,
                        raft_cmdpb::AdminCmdType_Name(type));
            return EngineStoreApplyRes::NotFound;
        }

        auto & curr_region = *curr_region_ptr;

        // Admin cmd contains no normal data, we can advance orphan keys info just before handling.
        if (curr_region.getClusterRaftstoreVer() == RaftstoreVer::V2)
        {
            curr_region.orphanKeysInfo().advanceAppliedIndex(index);
        }

        curr_region.makeRaftCommandDelegate(task_lock).handleAdminRaftCmd(
            request,
            response,
            index,
            term,
            *this,
            region_table,
            *raft_cmd_res);
        RaftCommandResult & result = *raft_cmd_res;

        // After region split / merge, try to flush it
        const auto try_to_flush_region = [&tmt](const RegionPtr & region) {
            try
            {
                tmt.getRegionTable().tryWriteBlockByRegionAndFlush(region, false);
            }
            catch (...)
            {
                tryLogCurrentException(__PRETTY_FUNCTION__);
            }
        };

        const auto persist_and_sync = [&](const Region & region) {
            tryFlushRegionCacheInStorage(tmt, region, log);
            persistRegion(region, &region_task_lock, PersistRegionReason::AdminCommand, "");
        };

        const auto handle_batch_split = [&](Regions & split_regions) {
            {
                // `split_regions` doesn't include the derived region.
                auto manage_lock = genRegionMgrWriteLock(task_lock);

                for (auto & new_region : split_regions)
                {
                    auto [it, ok] = manage_lock.regions.emplace(new_region->id(), new_region);
                    if (!ok)
                    {
                        // definitely, any region's index is greater or equal than the initial one.

                        // if there is already a region with same id, it means program crashed while persisting.
                        // just use the previous one.
                        new_region = it->second;
                    }
                }

                manage_lock.index.remove(result.ori_region_range->comparableKeys(), curr_region_id);
                manage_lock.index.add(curr_region_ptr);

                for (auto & new_region : split_regions)
                    manage_lock.index.add(new_region);
            }

            {
                // update region_table first is safe, because the core rule is established: the range in RegionTable
                // is always >= range in KVStore.
                for (const auto & new_region : split_regions)
                    region_table.updateRegion(*new_region);
                region_table.shrinkRegionRange(curr_region);
            }

            {
                for (const auto & new_region : split_regions)
                    try_to_flush_region(new_region);
            }

            {
                // persist curr_region at last. if program crashed after split_region is persisted, curr_region can
                // continue to complete split operation.
                for (const auto & new_region : split_regions)
                {
                    // no need to lock those new regions, because they don't have middle state.
                    persist_and_sync(*new_region);
                }
                persist_and_sync(curr_region);
            }
        };

        const auto handle_change_peer = [&]() {
            if (curr_region.isPendingRemove())
            {
                // remove `curr_region` from this node, we can remove its data.
                removeRegion(curr_region_id, /* remove_data */ true, region_table, task_lock, region_task_lock);
            }
            else
                persist_and_sync(curr_region);
        };

        const auto handle_commit_merge = [&](const RegionID source_region_id) {
            region_table.shrinkRegionRange(curr_region);
            try_to_flush_region(curr_region_ptr);
            persist_and_sync(curr_region);
            {
                auto source_region = getRegion(source_region_id);
                // `source_region` is merged, don't remove its data in storage.
                removeRegion(
                    source_region_id,
                    /* remove_data */ false,
                    region_table,
                    task_lock,
                    region_manager.genRegionTaskLock(source_region_id));
            }
            {
                auto manage_lock = genRegionMgrWriteLock(task_lock);
                manage_lock.index.remove(result.ori_region_range->comparableKeys(), curr_region_id);
                manage_lock.index.add(curr_region_ptr);
            }
        };

        switch (result.type)
        {
        case RaftCommandResult::Type::IndexError:
        {
            if (type == raft_cmdpb::AdminCmdType::CommitMerge)
            {
                if (auto source_region = getRegion(request.commit_merge().source().id()); source_region)
                {
                    LOG_WARNING(log,
                                "Admin cmd {} has been applied, try to remove source {}",
                                raft_cmdpb::AdminCmdType_Name(type),
                                source_region->toString(false));
                    source_region->setPendingRemove();
                    // `source_region` is merged, don't remove its data in storage.
                    removeRegion(source_region->id(), /* remove_data */ false, region_table, task_lock, region_manager.genRegionTaskLock(source_region->id()));
                }
            }
            break;
        }
        case RaftCommandResult::Type::BatchSplit:
            handle_batch_split(result.split_regions);
            break;
        case RaftCommandResult::Type::Default:
            persist_and_sync(curr_region);
            break;
        case RaftCommandResult::Type::ChangePeer:
            handle_change_peer();
            break;
        case RaftCommandResult::Type::CommitMerge:
            handle_commit_merge(result.source_region_id);
            break;
        }

        return EngineStoreApplyRes::Persist;
    }
}

void WaitCheckRegionReady(
    const TMTContext & tmt,
    KVStore & kvstore,
    const std::atomic_size_t & terminate_signals_counter,
    double wait_tick_time,
    double max_wait_tick_time,
    double get_wait_region_ready_timeout_sec)
{
    constexpr double batch_read_index_time_rate = 0.2; // part of time for waiting shall be assigned to batch-read-index
    auto log = Logger::get(__FUNCTION__);

    LOG_INFO(log,
             "start to check regions ready, min-wait-tick {}s, max-wait-tick {}s, wait-region-ready-timeout {:.3f}s",
             wait_tick_time,
             max_wait_tick_time,
             get_wait_region_ready_timeout_sec);

    std::unordered_set<RegionID> remain_regions;
    std::unordered_map<RegionID, uint64_t> regions_to_check;
    Stopwatch region_check_watch;
    size_t total_regions_cnt = 0;
    {
        kvstore.traverseRegions([&remain_regions](RegionID region_id, const RegionPtr &) { remain_regions.emplace(region_id); });
        total_regions_cnt = remain_regions.size();
    }
    while (region_check_watch.elapsedSeconds() < get_wait_region_ready_timeout_sec * batch_read_index_time_rate
           && terminate_signals_counter.load(std::memory_order_relaxed) == 0)
    {
        std::vector<kvrpcpb::ReadIndexRequest> batch_read_index_req;
        for (auto it = remain_regions.begin(); it != remain_regions.end();)
        {
            auto region_id = *it;
            if (auto region = kvstore.getRegion(region_id); region)
            {
                batch_read_index_req.emplace_back(GenRegionReadIndexReq(*region));
                it++;
            }
            else
            {
                it = remain_regions.erase(it);
            }
        }
        auto read_index_res = kvstore.batchReadIndex(batch_read_index_req, tmt.batchReadIndexTimeout());
        for (auto && [resp, region_id] : read_index_res)
        {
            bool need_retry = resp.read_index() == 0;
            if (resp.has_region_error())
            {
                const auto & region_error = resp.region_error();
                if (region_error.has_region_not_found() || region_error.has_epoch_not_match())
                    need_retry = false;
                LOG_DEBUG(log,
                          "neglect error region {} not found {} epoch not match {}",
                          region_id,
                          region_error.has_region_not_found(),
                          region_error.has_epoch_not_match());
            }
            if (!need_retry)
            {
                // `read_index` can be zero if region error happens.
                // It is not worthy waiting applying and reading index again.
                // if region is able to get latest commit-index from TiKV, we should make it available only after it has caught up.
                regions_to_check.emplace(region_id, resp.read_index());
                remain_regions.erase(region_id);
            }
            else
            {
                // retry in next round
            }
        }
        if (remain_regions.empty())
            break;

        LOG_INFO(log,
                 "{} regions need to fetch latest commit-index in next round, sleep for {:.3f}s",
                 remain_regions.size(),
                 wait_tick_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(wait_tick_time * 1000)));
        wait_tick_time = std::min(max_wait_tick_time, wait_tick_time * 2);
    }

    if (!remain_regions.empty())
    {
        FmtBuffer buffer;
        buffer.joinStr(
            remain_regions.begin(),
            remain_regions.end(),
            [&](const auto & e, FmtBuffer & b) { b.fmtAppend("{}", e); },
            " ");
        LOG_WARNING(
            log,
            "{} regions CANNOT fetch latest commit-index from TiKV, (region-id): {}",
            remain_regions.size(),
            buffer.toString());
    }
    do
    {
        for (auto it = regions_to_check.begin(); it != regions_to_check.end();)
        {
            auto [region_id, latest_index] = *it;
            if (auto region = kvstore.getRegion(region_id); region)
            {
                if (region->appliedIndex() >= latest_index)
                {
                    it = regions_to_check.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            else
            {
                it = regions_to_check.erase(it);
            }
        }

        if (regions_to_check.empty())
            break;

        LOG_INFO(log,
                 "{} regions need to apply to latest index, sleep for {:.3f}s",
                 regions_to_check.size(),
                 wait_tick_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(wait_tick_time * 1000)));
        wait_tick_time = std::min(max_wait_tick_time, wait_tick_time * 2);
    } while (region_check_watch.elapsedSeconds() < get_wait_region_ready_timeout_sec
             && terminate_signals_counter.load(std::memory_order_relaxed) == 0);

    if (!regions_to_check.empty())
    {
        FmtBuffer buffer;
        buffer.joinStr(
            regions_to_check.begin(),
            regions_to_check.end(),
            [&](const auto & e, FmtBuffer & b) {
                if (auto r = kvstore.getRegion(e.first); r)
                {
                    b.fmtAppend("{},{},{}", e.first, e.second, r->appliedIndex());
                }
                else
                {
                    b.fmtAppend("{},{},none", e.first, e.second);
                }
            },
            " ");
        LOG_WARNING(log, "{} regions CANNOT catch up with latest index, (region-id,latest-index,apply-index): {}", regions_to_check.size(), buffer.toString());
    }

    LOG_INFO(log,
             "finish to check {} regions, time cost {:.3f}s",
             total_regions_cnt,
             region_check_watch.elapsedSeconds());
}

void WaitCheckRegionReady(const TMTContext & tmt, KVStore & kvstore, const std::atomic_size_t & terminate_signals_counter)
{
    // wait interval to check region ready, not recommended to modify only if for tesing
    auto wait_region_ready_tick = tmt.getContext().getConfigRef().getUInt64("flash.wait_region_ready_tick", 0);
    auto wait_region_ready_timeout_sec = static_cast<double>(tmt.waitRegionReadyTimeout());
    const double max_wait_tick_time = 0 == wait_region_ready_tick ? 20.0 : wait_region_ready_timeout_sec;
    double min_wait_tick_time = 0 == wait_region_ready_tick ? 2.5 : static_cast<double>(wait_region_ready_tick); // default tick in TiKV is about 2s (without hibernate-region)
    return WaitCheckRegionReady(tmt, kvstore, terminate_signals_counter, min_wait_tick_time, max_wait_tick_time, wait_region_ready_timeout_sec);
}

void KVStore::setStore(metapb::Store store_)
{
    getStore().update(std::move(store_));
    LOG_INFO(log, "Set store info {}", getStore().base.ShortDebugString());
}

StoreID KVStore::getStoreID(std::memory_order memory_order) const
{
    return getStore().store_id.load(memory_order);
}

KVStore::StoreMeta::Base KVStore::StoreMeta::getMeta() const
{
    std::lock_guard lock(mu);
    return base;
}

metapb::Store KVStore::getStoreMeta() const
{
    return getStore().getMeta();
}

KVStore::StoreMeta & KVStore::getStore()
{
    return this->store;
}

const KVStore::StoreMeta & KVStore::getStore() const
{
    return this->store;
}

void KVStore::StoreMeta::update(Base && base_)
{
    std::lock_guard lock(mu);
    base = std::move(base_);
    store_id = base.id();
}

KVStore::~KVStore()
{
    releaseReadIndexWorkers();
}

FileUsageStatistics KVStore::getFileUsageStatistics() const
{
    if (!region_persister)
    {
        return {};
    }

    return region_persister->getFileUsageStatistics();
}

void KVStore::proactiveFlushCacheAndRegion(TMTContext & tmt, const DM::RowKeyRange & rowkey_range, KeyspaceID keyspace_id, TableID table_id, bool is_background)
{
    Stopwatch general_watch;
    UInt64 total_dm_flush_millis = 0;
    SCOPE_EXIT({
        if (is_background)
        {
            GET_METRIC(tiflash_storage_subtask_count, type_compact_log_segment_bg).Increment();
            GET_METRIC(tiflash_storage_subtask_duration_seconds, type_compact_log_bg).Observe(general_watch.elapsedSeconds());
            GET_METRIC(tiflash_storage_subtask_duration_seconds, type_compact_log_bg_dm).Observe(total_dm_flush_millis / 1000.0);
        }
        else
        {
            GET_METRIC(tiflash_storage_subtask_count, type_compact_log_segment_fg).Increment();
            GET_METRIC(tiflash_storage_subtask_duration_seconds, type_compact_log_fg).Observe(general_watch.elapsedSeconds());
            GET_METRIC(tiflash_storage_subtask_duration_seconds, type_compact_log_fg_dm).Observe(total_dm_flush_millis / 1000.0);
        }
    });

    auto storage = tmt.getStorages().get(keyspace_id, table_id);
    if (unlikely(storage == nullptr))
    {
        LOG_WARNING(log,
                    "proactiveFlushCacheAndRegion can not get table for table id {}, ignored",
                    table_id);
        return;
    }
    auto range = std::make_pair(TiKVRangeKey::makeTiKVRangeKey<true>(RecordKVFormat::encodeAsTiKVKey(*rowkey_range.start.toRegionKey(table_id))),
                                TiKVRangeKey::makeTiKVRangeKey<false>(RecordKVFormat::encodeAsTiKVKey(*rowkey_range.end.toRegionKey(table_id))));
    Stopwatch watch;

    LOG_INFO(log, "Start proactive flush region range [{},{}] [table_id={}] [keyspace_id={}] [is_background={}]", range.first.toDebugString(), range.second.toDebugString(), table_id, keyspace_id, is_background);
    /// It finds r1,r2,r3 in the following case.
    ///     |------ range ------|
    /// |--- r1 ---|--- r2 ---|--- r3 ---|
    struct RegionCompactInfo
    {
        UInt64 applied_index;
        UInt64 applied_term;
        DM::RowKeyRange rowkey_range;
        RegionPtr region_ptr;
        bool skip_flush;
    };
    std::unordered_map<UInt64, RegionCompactInfo> region_compact_indexes;
    {
        // Get the regions.
        auto task_lock = genTaskLock();
        auto maybe_region_map = [&]() {
            auto manage_lock = genRegionMgrReadLock();
            // Check if the region overlaps.
            return manage_lock.index.findByRangeChecked(range);
        }();

        if (std::holds_alternative<RegionsRangeIndex::OverlapInfo>(maybe_region_map))
        {
            auto & info = std::get<RegionsRangeIndex::OverlapInfo>(maybe_region_map);
            FmtBuffer buffer;
            buffer.joinStr(
                std::get<1>(info).begin(),
                std::get<1>(info).end(),
                [&](const auto & e, FmtBuffer & b) { b.fmtAppend("{}", e); },
                " ");
            std::string fmt_error = fmt::format("Find overlapped regions at {}, regions are {}, quit", std::get<0>(info).toDebugString(), buffer.toString());
            LOG_ERROR(log, fmt_error);
            throw Exception(fmt_error, ErrorCodes::LOGICAL_ERROR);
        }

        auto & region_map = std::get<RegionMap>(maybe_region_map);
        for (const auto & overlapped_region : region_map)
        {
            auto region_rowkey_range = DM::RowKeyRange::fromRegionRange(
                overlapped_region.second->getRange(),
                table_id,
                storage->isCommonHandle(),
                storage->getRowKeyColumnSize());
            region_compact_indexes[overlapped_region.first] = {
                overlapped_region.second->appliedIndex(),
                overlapped_region.second->appliedIndexTerm(),
                region_rowkey_range,
                overlapped_region.second,
                false};
        }
    }
    FAIL_POINT_PAUSE(FailPoints::pause_proactive_flush_before_persist_region);
    // Flush all segments in the range of regions.
    // TODO: combine adjacent range to do one flush.
    std::string reason = fmt::format("proactiveFlush{}", is_background ? "Bg" : "Fg");
    for (auto & region_compact_info : region_compact_indexes)
    {
        const auto & region_rowkey_range = region_compact_info.second.rowkey_range;
        auto region_id = region_compact_info.first;
        auto region_ptr = region_compact_info.second.region_ptr;
        auto applied_index = region_compact_info.second.applied_index;
        auto last_flushed_applied = region_ptr->lastCompactLogApplied();
        {
            auto region_task_lock = region_manager.genRegionTaskLock(region_id);
            enum class SkipReason
            {
                None = 0,
                Time = 1,
                Log = 2
            };
            SkipReason skip_reason = SkipReason::None;
            if (region_ptr->lastCompactLogTime() + Seconds{region_compact_log_period.load(std::memory_order_relaxed)} > Clock::now())
            {
                skip_reason = SkipReason::Time;
            }
            else if (region_ptr->lastCompactLogApplied() + 15 < applied_index)
            {
                skip_reason = SkipReason::Log;
            }
            if (skip_reason != SkipReason::None)
            {
                LOG_INFO(log, "skip flush region {} for skip reason {}, region range:[{},{}], flushed segment range:[{},{}]", region_id, magic_enum::enum_name(skip_reason), region_rowkey_range.getStart().toDebugString(), region_rowkey_range.getEnd().toDebugString(), rowkey_range.getStart().toDebugString(), rowkey_range.getEnd().toDebugString());
                region_compact_info.second.skip_flush = true;
                continue;
            }
            // Both flushCache and persistRegion should be protected by region task lock.
            // We can avoid flushCache with a region lock held, if we save some meta info before flushing cache in memory.
            // After flushCache, we will persist region and notify Proxy with the previously stored meta info.
            // Meanwhile, other write/admin cmds may be executed, we have to handle the following cases:
            // For write cmds, we need to support replay from KVStore level, like enhancing duplicate key detection.
            // For admin cmds, it can cause insertion/deletion of regions, so it can't be replayed currently.
            // Merely persisting applied_index is not enough, consider some cmds leads to modification of other meta data.

            if (rowkey_range.getStart() <= region_rowkey_range.getStart()
                && region_rowkey_range.getEnd() <= rowkey_range.getEnd()
                && last_flushed_applied >= applied_index)
            {
                // `region_rowkey_range` belongs to rowkey_range.
                // E.g. [0,9223372036854775807] belongs to [-9223372036854775808,9223372036854775807].
                // This segment has been flushed, and the region is locked.
                // However, writes may come between we lock regions.

                // TODO We can save the applied_index of every region, before the last time we flushCache.
                // And we will persistRegion according to this applied_index, following the upper note.
                LOG_DEBUG(log, "segment of region {} flushed, [applied_index={}] [applied_term={}] [last_flushed_applied={}]", region_compact_info.first, region_compact_info.second.applied_index, region_compact_info.second.applied_term, last_flushed_applied);
            }
            else
            {
                Stopwatch watch2;
                LOG_DEBUG(log, "extra segment of region {} to flush, region range:[{},{}], flushed segment range:[{},{}] [last_flushed_applied={}]", region_compact_info.first, region_rowkey_range.getStart().toDebugString(), region_rowkey_range.getEnd().toDebugString(), rowkey_range.getStart().toDebugString(), rowkey_range.getEnd().toDebugString(), last_flushed_applied);
                storage->flushCache(tmt.getContext(), region_rowkey_range);
                total_dm_flush_millis += watch2.elapsedSecondsFromLastTime();
            }
            fiu_do_on(FailPoints::proactive_flush_between_persist_cache_and_region, return;);
            {
                persistRegion(*region_ptr, std::make_optional(&region_task_lock), PersistRegionReason::ProactiveFlush, reason.c_str());
                // So proxy can get the current compact state of this region of TiFlash's side.
                region_ptr->markCompactLog();
                region_ptr->cleanApproxMemCacheInfo();
            }
            // Drop region task lock.
        }
        // Flush the origin cache again, in case writes has been written between handing regions.
        storage->flushCache(tmt.getContext(), rowkey_range);
        SYNC_FOR("after_proactiveFlushCacheAndRegion::loop_region");
    }
    auto elapsed_coupled_flush = watch.elapsedMilliseconds();
    watch.restart();

    fiu_do_on(FailPoints::proactive_flush_between_persist_regions, return;);
    // forbid regions being removed.
    for (const auto & region_compact_info : region_compact_indexes)
    {
        // Can truncated to flushed index, which is applied_index in this case.
        // Region can be removed since we don't lock kvstore here.
        if (!region_compact_info.second.skip_flush)
        {
            auto region_id = region_compact_info.first;
            LOG_DEBUG(log, "skip notify compactlog region {}", region_id);
            notifyCompactLog(region_id, region_compact_info.second.applied_index, region_compact_info.second.applied_term, is_background, false);
        }
    }
    auto elapsed_notify_proxy = watch.elapsedMilliseconds();

    LOG_DEBUG(log, "Finished proactive flush region range [{},{}] of {} regions. [couple_flush={}] [notify_proxy={}] [table_id={}] [keyspace_id={}] [is_background={}]", range.first.toDebugString(), range.second.toDebugString(), region_compact_indexes.size(), elapsed_coupled_flush, elapsed_notify_proxy, table_id, keyspace_id, is_background);
}

/// The function will notify Proxy to schedule a CheckCompact task. When this task is handled,
/// When handling, it will try to update `applied_index` and `truncated_state`, and then persist.
/// The updated `truncated_state` will not exceed the recorded `max_compact_index`.
/// `max_compact_index` is updated by CompactLog, whether it is filtered.
/// Requirements:
/// 1. The caller will guarantee that delta cache has been flushed.
/// 1. Region cache being persisted before notifying.
/// The truncated_index is mono-increase since:
/// 1. Every non-filtered passive flush uses what is from the `CompactLog`,
///    and `entry_storage::first_index`/`compact_raft_log` will guard that.
/// 1. Every proactive flush uses the newest `applied_index`.
void KVStore::notifyCompactLog(RegionID region_id, UInt64 compact_index, UInt64 compact_term, bool is_background, bool lock_held)
{
    auto region = getRegion(region_id);
    if (!region)
    {
        LOG_INFO(log, "region {} has been removed, ignore", region_id);
        return;
    }

    if (is_background)
    {
        GET_METRIC(tiflash_storage_subtask_count, type_compact_log_region_bg).Increment();
    }
    else
    {
        GET_METRIC(tiflash_storage_subtask_count, type_compact_log_region_fg).Increment();
    }
    auto f = [&]() {
        // We will notify even if `flush_state.applied_index` is greater than `compact_index`,
        // since this greater `applied_index` may not trigger a compact log.
        // We will maintain the biggest on Proxy's side.
        getProxyHelper()->notifyCompactLog(region_id, compact_index, compact_term, compact_index);
    };
    if (lock_held)
    {
        f();
    }
    else
    {
        auto region_task_lock = region_manager.genRegionTaskLock(region_id);
        f();
    }
}
} // namespace DB
