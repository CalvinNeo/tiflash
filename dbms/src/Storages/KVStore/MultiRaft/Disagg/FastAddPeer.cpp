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

#include <Common/FailPoint.h>
#include <Common/SyncPoint/SyncPoint.h>
#include <Common/TiFlashMetrics.h>
#include <Common/setThreadName.h>
#include <Encryption/PosixRandomAccessFile.h>
#include <Interpreters/Context.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/KVStore/FFI/ProxyFFI.h>
#include <Storages/KVStore/FFI/ProxyFFICommon.h>
#include <Storages/KVStore/KVStore.h>
#include <Storages/KVStore/MultiRaft/Disagg/CheckpointInfo.h>
#include <Storages/KVStore/MultiRaft/Disagg/CheckpointIngestInfo.h>
#include <Storages/KVStore/MultiRaft/Disagg/FastAddPeer.h>
#include <Storages/KVStore/MultiRaft/Disagg/FastAddPeerCache.h>
#include <Storages/KVStore/Region.h>
#include <Storages/KVStore/TMTContext.h>
#include <Storages/KVStore/Utils/AsyncTasks.h>
#include <Storages/Page/Config.h>
#include <Storages/Page/V3/CheckpointFile/CPManifestFileReader.h>
#include <Storages/Page/V3/PageDirectory.h>
#include <Storages/Page/V3/Universal/RaftDataReader.h>
#include <Storages/Page/V3/Universal/UniversalPageIdFormatImpl.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>
#include <Storages/Page/V3/Universal/UniversalWriteBatchImpl.h>
#include <Storages/S3/CheckpointManifestS3Set.h>
#include <Storages/S3/S3Common.h>
#include <Storages/S3/S3Filename.h>
#include <Storages/S3/S3GCManager.h>
#include <Storages/S3/S3RandomAccessFile.h>
#include <Storages/StorageDeltaMerge.h>
#include <fmt/core.h>

#include <memory>

namespace DB
{

namespace FailPoints
{
extern const char force_set_fap_candidate_store_id[];
} // namespace FailPoints

FastAddPeerRes genFastAddPeerRes(FastAddPeerStatus status, std::string && apply_str, std::string && region_str)
{
    auto * apply = RawCppString::New(apply_str);
    auto * region = RawCppString::New(region_str);
    return FastAddPeerRes{
        .status = status,
        .apply_state = CppStrWithView{.inner = GenRawCppPtr(apply, RawCppPtrTypeImpl::String), .view = BaseBuffView{apply->data(), apply->size()}},
        .region = CppStrWithView{.inner = GenRawCppPtr(region, RawCppPtrTypeImpl::String), .view = BaseBuffView{region->data(), region->size()}},
    };
}

std::vector<StoreID> getCandidateStoreIDsForRegion(TMTContext & tmt_context, UInt64 region_id, UInt64 current_store_id)
{
    auto pd_client = tmt_context.getPDClient();
    auto resp = pd_client->getRegionByID(region_id);
    const auto & region = resp.region();
    std::vector<StoreID> store_ids;
    store_ids.reserve(region.peers_size());
    for (const auto & peer : region.peers())
    {
        if (peer.store_id() == current_store_id)
            continue;
        // TODO: use label on the store to determine whether it's tiflash WN
        if (peer.role() == metapb::PeerRole::Learner)
        {
            store_ids.push_back(peer.store_id());
        }
    }
    return store_ids;
}

using raft_serverpb::PeerState;
using raft_serverpb::RaftApplyState;
using raft_serverpb::RegionLocalState;


std::optional<CheckpointRegionInfoAndData> tryParseRegionInfoFromCheckpointData(
    ParsedCheckpointDataHolderPtr checkpoint_data_holder,
    UInt64 remote_store_id,
    UInt64 region_id,
    const TiFlashRaftProxyHelper * proxy_helper)
{
    auto * log = &Poco::Logger::get("FastAddPeer");
    RegionPtr region;
    {
        auto region_key = UniversalPageIdFormat::toKVStoreKey(region_id);
        auto page = checkpoint_data_holder->getUniversalPageStorage()
                        ->read(region_key, /*read_limiter*/ nullptr, {}, /*throw_on_not_exist*/ false);
        if (page.isValid())
        {
            ReadBufferFromMemory buf(page.data.begin(), page.data.size());
            region = Region::deserialize(buf, proxy_helper);
        }
        else
        {
            GET_METRIC(tiflash_fap_nomatch_reason, type_no_meta).Increment();
            return std::nullopt;
        }
    }

    RaftApplyState apply_state;
    {
        // TODO: use `RaftDataReader::readRegionApplyState`?
        auto apply_state_key = UniversalPageIdFormat::toRaftApplyStateKeyInKVEngine(region_id);
        auto page = checkpoint_data_holder->getUniversalPageStorage()
                        ->read(apply_state_key, /*read_limiter*/ nullptr, {}, /*throw_on_not_exist*/ false);
        if (page.isValid())
        {
            apply_state.ParseFromArray(page.data.begin(), page.data.size());
        }
        else
        {
            GET_METRIC(tiflash_fap_nomatch_reason, type_no_meta).Increment();
            LOG_DEBUG(log, "Failed to find apply state key, region_id={}", region_id);
            return std::nullopt;
        }
    }

    RegionLocalState region_state;
    {
        auto local_state_key = UniversalPageIdFormat::toRegionLocalStateKeyInKVEngine(region_id);
        auto page = checkpoint_data_holder->getUniversalPageStorage()
                        ->read(local_state_key, /*read_limiter*/ nullptr, {}, /*throw_on_not_exist*/ false);
        if (page.isValid())
        {
            region_state.ParseFromArray(page.data.begin(), page.data.size());
        }
        else
        {
            GET_METRIC(tiflash_fap_nomatch_reason, type_no_meta).Increment();
            LOG_DEBUG(log, "Failed to find region local state key region_id={}", region_id);
            return std::nullopt;
        }
    }

    auto checkpoint_info = std::make_shared<CheckpointInfo>();
    checkpoint_info->remote_store_id = remote_store_id;
    checkpoint_info->region_id = region_id;
    checkpoint_info->checkpoint_data_holder = checkpoint_data_holder;
    checkpoint_info->temp_ps = checkpoint_data_holder->getUniversalPageStorage();
    return std::make_tuple(checkpoint_info, region, apply_state, region_state);
}

bool tryResetPeerIdInRegion(RegionPtr region, const RegionLocalState & region_state, uint64_t new_peer_id)
{
    auto peer_state = region_state.state();
    if (peer_state == PeerState::Tombstone || peer_state == PeerState::Applying)
    {
        GET_METRIC(tiflash_fap_nomatch_reason, type_region_state).Increment();
        return false;
    }
    for (const auto & peer : region_state.region().peers())
    {
        if (peer.id() == new_peer_id)
        {
            auto peer_copy = peer;
            region->mutMeta().setPeer(std::move(peer_copy));
            return true;
        }
    }
    GET_METRIC(tiflash_fap_nomatch_reason, type_conf).Increment();
    return false;
}

std::variant<CheckpointRegionInfoAndData, FastAddPeerRes> FastAddPeerImplSelect(
    TMTContext & tmt,
    const TiFlashRaftProxyHelper * proxy_helper,
    uint64_t region_id,
    uint64_t new_peer_id)
{
    GET_METRIC(tiflash_fap_task_state, type_selecting_stage).Increment();
    SCOPE_EXIT({ GET_METRIC(tiflash_fap_task_state, type_selecting_stage).Decrement(); });

    auto log = Logger::get("FastAddPeer");
    Stopwatch watch;
    std::unordered_map<StoreID, UInt64> checked_seq_map;
    auto fap_ctx = tmt.getContext().getSharedContextDisagg()->fap_context;
    auto cancel_handle = fap_ctx->tasks_trace->getCancelHandleFromExecutor(region_id);

    // Get candidate stores.
    const auto & settings = tmt.getContext().getSettingsRef();
    auto current_store_id = tmt.getKVStore()->clonedStoreMeta().id();
    std::vector<StoreID> candidate_store_ids;
    fiu_do_on(FailPoints::force_set_fap_candidate_store_id, { candidate_store_ids = {1234}; });
    if (candidate_store_ids.empty())
        candidate_store_ids = getCandidateStoreIDsForRegion(tmt, region_id, current_store_id);
    if (candidate_store_ids.empty())
    {
        LOG_DEBUG(log, "No suitable candidate peer for region_id={}", region_id);
        GET_METRIC(tiflash_fap_task_result, type_failed_no_suitable).Increment();
        return genFastAddPeerRes(FastAddPeerStatus::NoSuitable, "", "");
    }
    LOG_DEBUG(log, "Begin to select checkpoint for region_id={}", region_id);

    // It will return with FastAddPeerRes or failed with timeout result wrapped in FastAddPeerRes.
    while (true)
    {
        // Check all candidate stores in this loop.
        for (const auto store_id : candidate_store_ids)
        {
            RUNTIME_CHECK(store_id != current_store_id, store_id, current_store_id);
            auto iter = checked_seq_map.find(store_id);
            auto checked_seq = (iter == checked_seq_map.end()) ? 0 : iter->second;
            auto [data_seq, checkpoint_data] = fap_ctx->getNewerCheckpointData(tmt.getContext(), store_id, checked_seq);

            checked_seq_map[store_id] = data_seq;
            if (data_seq > checked_seq)
            {
                RUNTIME_CHECK(checkpoint_data != nullptr);
                auto maybe_region_info
                    = tryParseRegionInfoFromCheckpointData(checkpoint_data, store_id, region_id, proxy_helper);
                if (!maybe_region_info.has_value())
                    continue;
                const auto & checkpoint_info = std::get<0>(maybe_region_info.value());
                auto & region = std::get<1>(maybe_region_info.value());
                auto & region_state = std::get<3>(maybe_region_info.value());
                if (tryResetPeerIdInRegion(region, region_state, new_peer_id))
                {
                    LOG_INFO(
                        log,
                        "Select checkpoint with data_seq={}, remote_store_id={} elapsed={} size(candidate_store_id)={} "
                        "region_id={}",
                        data_seq,
                        checkpoint_info->remote_store_id,
                        watch.elapsedSeconds(),
                        candidate_store_ids.size(),
                        region_id);
                    GET_METRIC(tiflash_fap_task_duration_seconds, type_select_stage).Observe(watch.elapsedSeconds());
                    return maybe_region_info.value();
                }
                else
                {
                    LOG_DEBUG(
                        log,
                        "Checkpoint with seq {} doesn't contain reusable region info region_id={} from_store_id={}",
                        data_seq,
                        region_id,
                        store_id);
                }
            }
        }
        {
            if (watch.elapsedSeconds() >= settings.fap_wait_checkpoint_timeout_seconds)
            {
                // This could happen if there are too many pending tasks in queue,
                LOG_INFO(
                    log,
                    "FastAddPeer timeout when select checkpoints region_id={} new_peer_id={}",
                    region_id,
                    new_peer_id);
                GET_METRIC(tiflash_fap_task_result, type_failed_timeout).Increment();
                return genFastAddPeerRes(FastAddPeerStatus::NoSuitable, "", "");
            }
            SYNC_FOR("in_FastAddPeerImplSelect::before_sleep");
            if (cancel_handle->blockedWaitFor(std::chrono::milliseconds(1000)))
            {
                LOG_INFO(log, "Cancel FAP during peer selecting, region_id={}", region_id);
                // Just remove the task from AsyncTasks, it will not write anything in disk during this stage.
                // NOTE once canceled, Proxy should no longer polling `FastAddPeer`, since it will result in `OtherError`.
                fap_ctx->tasks_trace->leakingDiscardTask(region_id);
                // We immediately increase this metrics when cancel, since a canceled task may not be fetched.
                GET_METRIC(tiflash_fap_task_result, type_failed_cancel).Increment();
                return genFastAddPeerRes(FastAddPeerStatus::Canceled, "", "");
            }
        }
    }
}

FastAddPeerRes FastAddPeerImplWrite(
    TMTContext & tmt,
    const TiFlashRaftProxyHelper * proxy_helper,
    UInt64 region_id,
    UInt64 new_peer_id,
    CheckpointRegionInfoAndData && checkpoint,
    UInt64 start_time)
{
    auto log = Logger::get("FastAddPeer");
    auto fap_ctx = tmt.getContext().getSharedContextDisagg()->fap_context;
    auto cancel_handle = fap_ctx->tasks_trace->getCancelHandleFromExecutor(region_id);
    const auto & settings = tmt.getContext().getSettingsRef();

    Stopwatch watch;
    SCOPE_EXIT({ GET_METRIC(tiflash_fap_task_duration_seconds, type_write_stage).Observe(watch.elapsedSeconds()); });
    GET_METRIC(tiflash_fap_task_state, type_writing_stage).Increment();
    SCOPE_EXIT({ GET_METRIC(tiflash_fap_task_state, type_writing_stage).Decrement(); });

    auto [checkpoint_info, region, apply_state, region_state] = checkpoint;

    auto & storages = tmt.getStorages();
    auto keyspace_id = region->getKeyspaceID();
    auto table_id = region->getMappedTableID();
    auto storage = storages.get(keyspace_id, table_id);
    if (!storage)
    {
        // TODO(fap) add some ddl syncing to prevent fallback.
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Can't get storage engine keyspace_id={} table_id={}",
            keyspace_id,
            table_id);
    }
    RUNTIME_CHECK_MSG(storage->engineType() == TiDB::StorageEngine::DT, "ingest into unsupported storage engine");
    auto dm_storage = std::dynamic_pointer_cast<StorageDeltaMerge>(storage);
    auto new_key_range = DM::RowKeyRange::fromRegionRange(
        region->getRange(),
        table_id,
        storage->isCommonHandle(),
        storage->getRowKeyColumnSize());

    if (cancel_handle->canceled())
    {
        LOG_INFO(log, "Cancel FAP before write, region_id={}", region_id);
        fap_ctx->cleanTask(tmt, proxy_helper, region_id, false);
        GET_METRIC(tiflash_fap_task_result, type_failed_cancel).Increment();
        return genFastAddPeerRes(FastAddPeerStatus::Canceled, "", "");
    }
    auto segments = dm_storage->buildSegmentsFromCheckpointInfo(new_key_range, checkpoint_info, settings);

    fap_ctx->insertCheckpointIngestInfo(
        tmt,
        region_id,
        new_peer_id,
        checkpoint_info->remote_store_id,
        region,
        std::move(segments),
        start_time);

    SYNC_FOR("in_FastAddPeerImplWrite::after_write_segments");
    if (cancel_handle->canceled())
    {
        LOG_INFO(log, "Cancel FAP after write segments, region_id={}", region_id);
        fap_ctx->cleanTask(tmt, proxy_helper, region_id, false);
        GET_METRIC(tiflash_fap_task_result, type_failed_cancel).Increment();
        return genFastAddPeerRes(FastAddPeerStatus::Canceled, "", "");
    }
    // Write raft log to uni ps, we do this here because we store raft log seperately.
    // Currently, FAP only handle when the peer is newly created in this store.
    // TODO(fap) However, Move this to `ApplyFapSnapshot` and clean stale data, if FAP can later handle all snapshots.
    UniversalWriteBatch wb;
    RaftDataReader raft_data_reader(*(checkpoint_info->temp_ps));
    raft_data_reader.traverseRemoteRaftLogForRegion(
        region_id,
        [&](const UniversalPageId & page_id, PageSize size, const PS::V3::CheckpointLocation & location) {
            LOG_DEBUG(
                log,
                "Write raft log size {}, region_id={} index={}",
                size,
                region_id,
                UniversalPageIdFormat::getU64ID(page_id));
            wb.putRemotePage(page_id, 0, size, location, {});
        });
    auto wn_ps = tmt.getContext().getWriteNodePageStorage();
    wn_ps->write(std::move(wb));
    SYNC_FOR("in_FastAddPeerImplWrite::after_write_raft_log");
    if (cancel_handle->canceled())
    {
        LOG_INFO(log, "Cancel FAP after write raft log, region_id={}", region_id);
        fap_ctx->cleanTask(tmt, proxy_helper, region_id, false);
        GET_METRIC(tiflash_fap_task_result, type_failed_cancel).Increment();
        return genFastAddPeerRes(FastAddPeerStatus::Canceled, "", "");
    }

    return genFastAddPeerRes(
        FastAddPeerStatus::Ok,
        apply_state.SerializeAsString(),
        region_state.region().SerializeAsString());
}

FastAddPeerRes FastAddPeerImpl(
    FastAddPeerContextPtr fap_ctx,
    TMTContext & tmt,
    const TiFlashRaftProxyHelper * proxy_helper,
    UInt64 region_id,
    UInt64 new_peer_id,
    UInt64 start_time)
{
    auto log = Logger::get("FastAddPeer");
    try
    {
        auto cancel_handle = fap_ctx->tasks_trace->getCancelHandleFromExecutor(region_id);
        if (cancel_handle->canceled())
        {
            LOG_INFO(log, "Cancel FAP in queue due to timeout region_id={} new_peer_id={}", region_id, new_peer_id);
            // It is already canceled in queue.
            GET_METRIC(tiflash_fap_task_result, type_failed_cancel).Increment();
            return genFastAddPeerRes(FastAddPeerStatus::Canceled, "", "");
        }
        auto elapsed = fap_ctx->tasks_trace->queryElapsed(region_id);
        GET_METRIC(tiflash_fap_task_duration_seconds, type_queue_stage).Observe(elapsed / 1000.0);
        GET_METRIC(tiflash_fap_task_state, type_queueing_stage).Decrement();
        auto res = FastAddPeerImplSelect(tmt, proxy_helper, region_id, new_peer_id);
        if (std::holds_alternative<CheckpointRegionInfoAndData>(res))
        {
            auto final_res = FastAddPeerImplWrite(
                tmt,
                proxy_helper,
                region_id,
                new_peer_id,
                std::move(std::get<CheckpointRegionInfoAndData>(res)),
                start_time);
            GET_METRIC(tiflash_fap_task_result, type_success_transform).Increment();
            return final_res;
        }
        return std::get<FastAddPeerRes>(res);
    }
    catch (const Exception & e)
    {
        // Could be like:
        // - can't put remote page with empty data_location
        DB::tryLogCurrentException(
            "FastAddPeerImpl",
            fmt::format(
                "Failed when try to restore from checkpoint region_id={} new_peer_id={} {}",
                region_id,
                new_peer_id,
                e.message()));
        GET_METRIC(tiflash_fap_task_result, type_failed_baddata).Increment();
        return genFastAddPeerRes(FastAddPeerStatus::BadData, "", "");
    }
    catch (...)
    {
        DB::tryLogCurrentException(
            "FastAddPeerImpl",
            fmt::format(
                "Failed when try to restore from checkpoint region_id={} new_peer_id={}",
                region_id,
                new_peer_id));
        GET_METRIC(tiflash_fap_task_result, type_failed_baddata).Increment();
        return genFastAddPeerRes(FastAddPeerStatus::BadData, "", "");
    }
}

uint8_t ApplyFapSnapshotImpl(TMTContext & tmt, TiFlashRaftProxyHelper * proxy_helper, UInt64 region_id, UInt64 peer_id)
{
    auto log = Logger::get("FastAddPeer");
    LOG_INFO(log, "Begin apply fap snapshot, region_id={}, peer_id={}", region_id, peer_id);
    Stopwatch watch_ingest;
    auto kvstore = tmt.getKVStore();
    auto fap_ctx = tmt.getContext().getSharedContextDisagg()->fap_context;
    auto checkpoint_ingest_info = fap_ctx->getOrRestoreCheckpointIngestInfo(tmt, proxy_helper, region_id, peer_id);
    if (!checkpoint_ingest_info)
    {
        return false;
    }
    // If there is `checkpoint_ingest_info`, it is exactly the data we want to ingest. Consider two scene:
    // 1. If there was a failed FAP which failed to clean, its data will be overwritten by current FAP which has finished phase 1.
    // 2. It is not possible that a restart happens at FAP phase 2, and a legacy snapshot is sent, because snapshots can only be accepted once the previous snapshot it handled.
    {
        GET_METRIC(tiflash_fap_task_state, type_ingesting_stage).Increment();
        SCOPE_EXIT({ GET_METRIC(tiflash_fap_task_state, type_ingesting_stage).Decrement(); });
        kvstore->handleIngestCheckpoint(checkpoint_ingest_info->getRegion(), checkpoint_ingest_info, tmt);
        fap_ctx->cleanTask(tmt, proxy_helper, region_id, true);
        GET_METRIC(tiflash_fap_task_duration_seconds, type_ingest_stage).Observe(watch_ingest.elapsedSeconds());
        auto begin = checkpoint_ingest_info->beginTime();
        auto current = FAPAsyncTasks::getCurrentMillis();
        if (begin != 0)
        {
            GET_METRIC(tiflash_fap_task_duration_seconds, type_total).Observe((current - begin) / 1000.0);
        }
        LOG_INFO(
            log,
            "Finish apply fap snapshot, region_id={} peer_id={} begin_time={} current_time={}",
            region_id,
            peer_id,
            begin,
            current);
        GET_METRIC(tiflash_fap_task_result, type_succeed).Increment();
        return true;
    }
}

uint8_t ApplyFapSnapshot(EngineStoreServerWrap * server, uint64_t region_id, uint64_t peer_id)
{
    try
    {
        RUNTIME_CHECK_MSG(server->tmt, "TMTContext is null");
        RUNTIME_CHECK_MSG(server->proxy_helper, "proxy_helper is null");
        if (!server->tmt->getContext().getSharedContextDisagg()->isDisaggregatedStorageMode())
            return false;
        return ApplyFapSnapshotImpl(*server->tmt, server->proxy_helper, region_id, peer_id);
    }
    catch (...)
    {
        DB::tryLogCurrentFatalException(
            "FastAddPeerApply",
            fmt::format("Failed when try to apply fap snapshot region_id={} peer_id={}", region_id, peer_id));
        exit(-1);
    }
}

FastAddPeerRes FastAddPeer(EngineStoreServerWrap * server, uint64_t region_id, uint64_t new_peer_id)
{
    try
    {
        auto log = Logger::get("FastAddPeer");
        if (!server->tmt->getContext().getSharedContextDisagg()->isDisaggregatedStorageMode())
            return genFastAddPeerRes(FastAddPeerStatus::OtherError, "", "");
        auto fap_ctx = server->tmt->getContext().getSharedContextDisagg()->fap_context;
        if (fap_ctx == nullptr)
        {
            LOG_WARNING(log, "FAP Context is not initialized. Should only enable FAP in DisaggregatedStorageMode.");
            return genFastAddPeerRes(FastAddPeerStatus::NoSuitable, "", "");
        }
        if (!fap_ctx->tasks_trace->isScheduled(region_id))
        {
            // We need to schedule the task.
            auto current_time = FAPAsyncTasks::getCurrentMillis();
            GET_METRIC(tiflash_fap_task_state, type_queueing_stage).Increment();
            auto res
                = fap_ctx->tasks_trace->addTask(region_id, [server, region_id, new_peer_id, fap_ctx, current_time]() {
                      std::string origin_name = getThreadName();
                      SCOPE_EXIT({ setThreadName(origin_name.c_str()); });
                      setThreadName("fap-builder");
                      return FastAddPeerImpl(
                          fap_ctx,
                          *(server->tmt),
                          server->proxy_helper,
                          region_id,
                          new_peer_id,
                          current_time);
                  });
            if (res)
            {
                GET_METRIC(tiflash_fap_task_state, type_ongoing).Increment();
                LOG_INFO(log, "Add new task success, new_peer_id={} region_id={}", new_peer_id, region_id);
            }
            else
            {
                // If the queue is full, the task won't be registered, return OtherError for quick fallback.
                // If proxy still mistakenly polls canceled task, it will also fails here.
                LOG_ERROR(
                    log,
                    "Add new task fail(queue full) or poll canceled, new_peer_id={} region_id={}",
                    new_peer_id,
                    region_id);
                GET_METRIC(tiflash_fap_task_result, type_failed_other).Increment();
                return genFastAddPeerRes(FastAddPeerStatus::OtherError, "", "");
            }
        }

        // If the task is canceled, the task will not be `isScheduled`.
        if (fap_ctx->tasks_trace->isReady(region_id))
        {
            GET_METRIC(tiflash_fap_task_state, type_ongoing).Decrement();
            auto [result, elapsed] = fap_ctx->tasks_trace->fetchResultAndElapsed(region_id);
            LOG_INFO(
                log,
                "Fetch task, result={} new_peer_id={} region_id={} elapsed={}",
                magic_enum::enum_name(result.status),
                new_peer_id,
                region_id,
                elapsed);
            GET_METRIC(tiflash_fap_task_result, type_total).Increment();
            GET_METRIC(tiflash_fap_task_duration_seconds, type_phase1_total).Observe(elapsed / 1000.0);
            return result;
        }
        else
        {
            const auto & settings = server->tmt->getContext().getSettingsRef();
            auto elapsed = fap_ctx->tasks_trace->queryElapsed(region_id);
            if (elapsed >= 1000 * settings.fap_task_timeout_seconds)
            {
                // For most cases, prev_state is `InQueue`.
                auto prev_state = fap_ctx->tasks_trace->asyncCancelTask(region_id);
                if (prev_state == FAPAsyncTasks::TaskState::InQueue)
                {
                    LOG_INFO(log, "Cancel FAP due to timeout region_id={} new_peer_id={}", region_id, new_peer_id);
                }
                else
                {
                    // If the task is running, we have to wait it return on cancel and clean,
                    // otherwise a later legacy may race with this clean.
                    GET_METRIC(tiflash_fap_task_state, type_blocking_cancel_stage).Increment();
                    [[maybe_unused]] auto result = fap_ctx->tasks_trace->fetchResult(region_id);
                    GET_METRIC(tiflash_fap_task_state, type_blocking_cancel_stage).Decrement();
                }
                // Return Canceled because it is cancel from outside FAP worker.
                return genFastAddPeerRes(FastAddPeerStatus::Canceled, "", "");
            }
            LOG_DEBUG(log, "Task is still pending new_peer_id={} region_id={}", new_peer_id, region_id);
            return genFastAddPeerRes(FastAddPeerStatus::WaitForData, "", "");
        }
    }
    catch (const Exception & e)
    {
        DB::tryLogCurrentException(
            "FastAddPeer",
            fmt::format(
                "Failed when try to restore from checkpoint region_id={} new_peer_id={} {}",
                region_id,
                new_peer_id,
                e.message()));
        return genFastAddPeerRes(FastAddPeerStatus::OtherError, "", "");
    }
    catch (...)
    {
        DB::tryLogCurrentException(
            "FastAddPeer",
            fmt::format(
                "Failed when try to restore from checkpoint region_id={} new_peer_id={}",
                region_id,
                new_peer_id));
        return genFastAddPeerRes(FastAddPeerStatus::OtherError, "", "");
    }
}
} // namespace DB
