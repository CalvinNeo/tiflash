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
#include <Common/TiFlashMetrics.h>
#include <Interpreters/Context.h>
#include <Storages/DeltaMerge/SSTFilesToBlockInputStream.h>
#include <Storages/DeltaMerge/SSTFilesToDTFilesOutputStream.h>
#include <Storages/KVStore/Decode/PartitionStreams.h>
#include <Storages/KVStore/FFI/ProxyFFI.h>
#include <Storages/KVStore/FFI/SSTReader.h>
#include <Storages/KVStore/KVStore.h>
#include <Storages/KVStore/Region.h>
#include <Storages/KVStore/TMTContext.h>
#include <Storages/KVStore/Types.h>
#include <Storages/KVStore/Utils/AsyncTasks.h>
#include <Storages/StorageDeltaMerge.h>
#include <Storages/StorageDeltaMergeHelpers.h>
#include <TiDB/Schema/SchemaSyncer.h>
#include <TiDB/Schema/TiDBSchemaManager.h>

namespace DB
{
namespace FailPoints
{
extern const char force_set_sst_to_dtfile_block_size[];
extern const char force_set_parallel_prehandle_threshold[];
extern const char force_raise_prehandle_exception[];
} // namespace FailPoints

namespace ErrorCodes
{
extern const int TABLE_IS_DROPPED;
extern const int REGION_DATA_SCHEMA_UPDATED;
} // namespace ErrorCodes

enum class ReadFromStreamError
{
    Ok,
    Aborted,
    ErrUpdateSchema,
    ErrTableDropped,
};

struct ReadFromStreamResult
{
    ReadFromStreamError error = ReadFromStreamError::Ok;
    std::string extra_msg;
};

static inline std::tuple<ReadFromStreamResult, PrehandleResult> executeTransform(
    const RegionPtr & new_region,
    const std::shared_ptr<std::atomic_bool> & prehandle_task,
    DM::FileConvertJobType job_type,
    const std::shared_ptr<StorageDeltaMerge> & storage,
    const std::shared_ptr<DM::SSTFilesToBlockInputStream> & sst_stream,
    const DM::SSTFilesToBlockInputStreamOpts & opts,
    TMTContext & tmt)
{
    auto region_id = new_region->id();
    std::shared_ptr<DM::SSTFilesToDTFilesOutputStream<DM::BoundedSSTFilesToBlockInputStreamPtr>> stream;
    // If any schema changes is detected during decoding SSTs to DTFiles, we need to cancel and recreate DTFiles with
    // the latest schema. Or we will get trouble in `BoundedSSTFilesToBlockInputStream`.
    try
    {
        auto & context = tmt.getContext();
        auto & global_settings = context.getGlobalContext().getSettingsRef();
        // Read from SSTs and refine the boundary of blocks output to DTFiles
        auto bounded_stream = std::make_shared<DM::BoundedSSTFilesToBlockInputStream>(
            sst_stream,
            ::DB::TiDBPkColumnID,
            opts.schema_snap);

        stream = std::make_shared<DM::SSTFilesToDTFilesOutputStream<DM::BoundedSSTFilesToBlockInputStreamPtr>>(
            opts.log_prefix,
            bounded_stream,
            storage,
            opts.schema_snap,
            job_type,
            /* split_after_rows */ global_settings.dt_segment_limit_rows,
            /* split_after_size */ global_settings.dt_segment_limit_size,
            region_id,
            prehandle_task,
            context);

        sst_stream->maybeSkipBySoftLimit();
        stream->writePrefix();
        fiu_do_on(FailPoints::force_raise_prehandle_exception, {
            if (auto v = FailPointHelper::getFailPointVal(FailPoints::force_raise_prehandle_exception); v)
            {
                auto flag = std::any_cast<std::shared_ptr<std::atomic_uint64_t>>(v.value());
                if(flag->load() == 1) {
                    flag->store(0);
                    throw Exception("fake exception once", ErrorCodes::REGION_DATA_SCHEMA_UPDATED);
                } else if (flag->load() == 2 ){
                    throw Exception("fake exception", ErrorCodes::REGION_DATA_SCHEMA_UPDATED);
                }
            }
        });
        stream->write();
        stream->writeSuffix();
        auto res = ReadFromStreamResult{.error = ReadFromStreamError::Ok, .extra_msg = ""};
        if (stream->isAbort())
        {
            stream->cancel();
            res = ReadFromStreamResult{.error = ReadFromStreamError::Aborted, .extra_msg = ""};
        }
        return std::make_pair(
            std::move(res),
            PrehandleResult{
                .ingest_ids = stream->outputFiles(),
                .stats = PrehandleResult::Stats{
                    .parallels = 1,
                    .raft_snapshot_bytes = sst_stream->getProcessKeys().total_bytes(),
                    .dt_disk_bytes = stream->getTotalBytesOnDisk(),
                    .dt_total_bytes = stream->getTotalCommittedBytes(),
                    .total_keys = sst_stream->getProcessKeys().total(),
                    .write_cf_keys = sst_stream->getProcessKeys().write_cf,
                    .max_split_write_cf_keys = sst_stream->getProcessKeys().write_cf
                }});
    }
    catch (DB::Exception & e)
    {
        if (stream != nullptr)
        {
            // Remove all DMFiles.
            stream->cancel();
        }
        if (e.code() == ErrorCodes::REGION_DATA_SCHEMA_UPDATED)
        {
            return std::make_pair(
                ReadFromStreamResult{.error = ReadFromStreamError::ErrUpdateSchema, .extra_msg = e.displayText()},
                PrehandleResult{});
        }
        else if (e.code() == ErrorCodes::TABLE_IS_DROPPED)
        {
            return std::make_pair(
                ReadFromStreamResult{.error = ReadFromStreamError::ErrTableDropped, .extra_msg = e.displayText()},
                PrehandleResult{});
        }
        throw;
    }
}

// It is currently a wrapper for preHandleSSTsToDTFiles.
PrehandleResult KVStore::preHandleSnapshotToFiles(
    RegionPtr new_region,
    const SSTViewVec snaps,
    uint64_t index,
    uint64_t term,
    std::optional<uint64_t> deadline_index,
    TMTContext & tmt)
{
    new_region->beforePrehandleSnapshot(new_region->id(), deadline_index);
    ongoing_prehandle_task_count.fetch_add(1);
    PrehandleResult result;
    try
    {
        SCOPE_EXIT({
            auto ongoing = ongoing_prehandle_task_count.fetch_sub(1) - 1;
            new_region->afterPrehandleSnapshot(ongoing);
        });
        result = preHandleSSTsToDTFiles( //
            new_region,
            snaps,
            index,
            term,
            DM::FileConvertJobType::ApplySnapshot,
            tmt);
    }
    catch (DB::Exception & e)
    {
        e.addMessage(
            fmt::format("(while preHandleSnapshot region_id={}, index={}, term={})", new_region->id(), index, term));
        e.rethrow();
    }
    return result;
}

static inline std::vector<std::string> getSplitKey(
    LoggerPtr log,
    KVStore * kvstore,
    RegionPtr new_region,
    std::shared_ptr<DM::SSTFilesToBlockInputStream> sst_stream)
{
    // We don't use this is the single snapshot is small, due to overhead in decoding.
    // TODO(split) find solution if the snapshot has too many untrimmed data.
    // TODO(split) recover this
    // constexpr size_t PARALLEL_PREHANDLE_THRESHOLD = 1 * 1024 * 1024 * 1024;
    constexpr size_t PARALLEL_PREHANDLE_THRESHOLD = 1 * 1024 * 1024;
    size_t parallel_prehandle_threshold = PARALLEL_PREHANDLE_THRESHOLD;
    fiu_do_on(FailPoints::force_set_parallel_prehandle_threshold, {
        if (auto v = FailPointHelper::getFailPointVal(FailPoints::force_set_parallel_prehandle_threshold); v)
            parallel_prehandle_threshold = std::any_cast<size_t>(v.value());
    });
    // If size is 0, do no parallel prehandle for this snapshot, which is legacy.
    // If size is non-zero, use extra this many threads to prehandle.
    std::vector<std::string> split_keys;
    // Don't change the order of following checks, `getApproxBytes` involves some overhead,
    // although it is optimized to bring about the minimum overhead.

    if (new_region->getClusterRaftstoreVer() == RaftstoreVer::V2 && kvstore->getOngoingPrehandleTaskCount() < 2
        && sst_stream->getApproxBytes() > parallel_prehandle_threshold)
    {
        // Get this info again, since getApproxBytes maybe take some time.
        auto ongoing_count = kvstore->getOngoingPrehandleTaskCount();
        const auto & proxy_config = kvstore->getProxyConfigSummay();
        uint64_t want_split_parts = 0;
        size_t total_concurrency = 0;
        if (proxy_config.valid)
        {
            total_concurrency = proxy_config.snap_handle_pool_size;
        }
        else
        {
            total_concurrency = std::thread::hardware_concurrency();
        }
        if (total_concurrency + 1 > ongoing_count)
        {
            // Current thread takes 1 which is in `ongoing_count`.
            // We use all threads to prehandle, since there is potentially no read and no delta merge when prehandling the only region.
            want_split_parts = total_concurrency - ongoing_count + 1;
        }

        if (want_split_parts > 1)
        {
            // Will generate at most `want_split_parts - 1` keys.
            split_keys = sst_stream->findSplitKeys(want_split_parts);

            RUNTIME_CHECK_MSG(
                split_keys.size() + 1 <= want_split_parts,
                "findSplitKeys should generate {} - 1 keys, actual {}",
                want_split_parts,
                split_keys.size());
            FmtBuffer fmt_buf;
            if (split_keys.size() + 1 < want_split_parts)
            {
                // If there are too few split keys, the `split_keys` itself may be not be uniformly distributed,
                // it is even better that we still handle it sequantially.
                split_keys.clear();
                LOG_INFO(
                    log,
                    "getSplitKey failed to split, ongoing={} want={} got={} region_id={}",
                    ongoing_count,
                    want_split_parts,
                    split_keys.size(),
                    new_region->id());
            }
            else
            {
                fmt_buf.joinStr(
                    split_keys.cbegin(),
                    split_keys.cend(),
                    [](const auto & arg, FmtBuffer & fb) {
                        // TODO(split) reduce copy here
                        fb.append(DecodedTiKVKey(std::string(arg)).toDebugString());
                    },
                    ":");
                LOG_INFO(
                    log,
                    "getSplitKey result {}, total_concurrency={} ongoing={} total_split_parts={} split_keys={} region_id={}",
                    fmt_buf.toString(),
                    total_concurrency,
                    ongoing_count,
                    want_split_parts,
                    split_keys.size(),
                    new_region->id());
            }
        }
        else
        {
            LOG_INFO(
                log,
                "getSplitKey refused to split, ongoing={} total_concurrency={} region_id={}",
                ongoing_count,
                total_concurrency,
                new_region->id());
        }
    }
    return split_keys;
}

/// `preHandleSSTsToDTFiles` read data from SSTFiles and generate DTFile(s) for commited data
/// return the ids of DTFile(s), the uncommitted data will be inserted to `new_region`
PrehandleResult KVStore::preHandleSSTsToDTFiles(
    RegionPtr new_region,
    const SSTViewVec snaps,
    uint64_t index,
    uint64_t term,
    DM::FileConvertJobType job_type,
    TMTContext & tmt)
{
    // if it's only a empty snapshot, we don't create the Storage object, but return directly.
    if (snaps.len == 0)
    {
        return {};
    }
    auto context = tmt.getContext();
    auto keyspace_id = new_region->getKeyspaceID();
    bool force_decode = false;
    size_t expected_block_size = DEFAULT_MERGE_BLOCK_SIZE;

    // Use failpoint to change the expected_block_size for some test cases
    fiu_do_on(FailPoints::force_set_sst_to_dtfile_block_size, {
        if (auto v = FailPointHelper::getFailPointVal(FailPoints::force_set_sst_to_dtfile_block_size); v)
            expected_block_size = std::any_cast<size_t>(v.value());
    });

    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_raft_command_duration_seconds, type_apply_snapshot_predecode)
            .Observe(watch.elapsedSeconds());
    });

    PrehandleResult prehandle_result;
    TableID physical_table_id = InvalidTableID;

    auto region_id = new_region->id();
    auto prehandle_task = prehandling_trace.registerTask(region_id);
    while (true)
    {
        // If any schema changes is detected during decoding SSTs to DTFiles, we need to cancel and recreate DTFiles with
        // the latest schema. Or we will get trouble in `BoundedSSTFilesToBlockInputStream`.
        try
        {
            // Get storage schema atomically, will do schema sync if the storage does not exists.
            // Will return the storage even if it is tombstone.
            const auto [table_drop_lock, storage, schema_snap] = AtomicGetStorageSchema(new_region, tmt);
            if (unlikely(storage == nullptr))
            {
                // The storage must be physically dropped, throw exception and do cleanup.
                throw Exception("", ErrorCodes::TABLE_IS_DROPPED);
            }

            // Get a gc safe point for compacting
            Timestamp gc_safepoint = 0;
            if (auto pd_client = tmt.getPDClient(); !pd_client->isMock())
            {
                gc_safepoint = PDClientHelper::getGCSafePointWithRetry(
                    pd_client,
                    keyspace_id,
                    /* ignore_cache= */ false,
                    context.getSettingsRef().safe_point_update_interval_seconds);
            }
            physical_table_id = storage->getTableInfo().id;

            auto opt = DM::SSTFilesToBlockInputStreamOpts{
                .log_prefix = fmt::format("keyspace={} table_id={}", keyspace_id, physical_table_id),
                .schema_snap = schema_snap,
                .gc_safepoint = gc_safepoint,
                .force_decode = force_decode,
                .expected_size = expected_block_size};

            auto sst_stream = std::make_shared<DM::SSTFilesToBlockInputStream>(
                new_region,
                index,
                snaps,
                proxy_helper,
                tmt,
                std::nullopt,
                DM::SSTFilesToBlockInputStreamOpts(opt));

            auto split_keys = getSplitKey(log, this, new_region, sst_stream);

            ReadFromStreamResult result;
            using SingleSnapshotAsyncTasks = AsyncTasks<uint64_t, std::function<bool()>, bool>;
            if (split_keys.empty())
            {
                LOG_INFO(
                    log,
                    "Single threaded prehandling for single big region, range={}, region_id={}",
                    new_region->getRange()->toDebugString(),
                    new_region->id());
                std::tie(result, prehandle_result)
                    = executeTransform(new_region, prehandle_task, job_type, storage, sst_stream, opt, tmt);
            }
            else
            {
                auto split_key_count = split_keys.size();
                RUNTIME_CHECK_MSG(
                    split_key_count >= 1,
                    "split_key_count should be more or equal than 1, actual {}",
                    split_key_count);
                LOG_INFO(
                    log,
                    "Parallel prehandling for single big region, range={}, split keys={}, region_id={}",
                    new_region->getRange()->toDebugString(),
                    split_key_count,
                    new_region->id());
                auto async_tasks = SingleSnapshotAsyncTasks(split_key_count, split_key_count, split_key_count + 5);
                sst_stream->resetSoftLimit(DM::SSTScanSoftLimit(
                    DM::SSTScanSoftLimit::HEAD_SPLIT,
                    std::string(""),
                    std::string(split_keys[0])));
                struct ParallelPrehandleCtx {
                    std::unordered_map<uint64_t, ReadFromStreamResult> gather_res;
                    std::unordered_map<uint64_t, PrehandleResult> gather_prehandle_res;
                    std::mutex mut;
                };
                using ParallelPrehandleCtxPtr = std::shared_ptr<ParallelPrehandleCtx>;
                ParallelPrehandleCtxPtr ctx = std::make_shared<ParallelPrehandleCtx>();
                auto runInParallel = [&](uint64_t extra_id,
                                         ParallelPrehandleCtxPtr ctx,
                                         DM::SSTScanSoftLimit && limit,
                                         std::shared_ptr<StorageDeltaMerge> dm_storage) {
                    try
                    {
                        std::string limit_tag = limit.toDebugString();
                        auto part_sst_stream = std::make_shared<DM::SSTFilesToBlockInputStream>(
                            new_region,
                            index,
                            snaps,
                            proxy_helper,
                            tmt,
                            std::move(limit),
                            DM::SSTFilesToBlockInputStreamOpts(opt));
                        auto [part_result, part_prehandle_result] = executeTransform(
                            new_region,
                            prehandle_task,
                            job_type,
                            dm_storage,
                            part_sst_stream,
                            opt,
                            tmt);
                        LOG_INFO(
                            log,
                            "Finished extra parallel prehandle task limit {} write cf {} dmfiles {} error {}, "
                            "split_id={} region_id={}",
                            limit_tag,
                            part_prehandle_result.stats.write_cf_keys,
                            part_prehandle_result.ingest_ids.size(),
                            magic_enum::enum_name(part_result.error),
                            extra_id,
                            new_region->id());
                        if (part_result.error == ReadFromStreamError::ErrUpdateSchema)
                        {
                            prehandle_task->store(true);
                        }
                        {
                            std::scoped_lock l(ctx->mut);
                            ctx->gather_res[extra_id] = std::move(part_result);
                            ctx->gather_prehandle_res[extra_id] = std::move(part_prehandle_result);
                        }
                    }
                    catch (Exception & e)
                    {
                        // The exception can be wrapped in the future, however, we abort here.
                        prehandle_task->store(true);
                        throw;
                    }
                };
                auto dm_storage = storage; // Make C++ happy.
                for (size_t extra_id = 0; extra_id < split_key_count; extra_id++)
                {
                    auto add_result = async_tasks.addTask(extra_id, [&, extra_id]() {
                        auto limit = DM::SSTScanSoftLimit(
                            extra_id,
                            std::string(split_keys[extra_id]),
                            extra_id + 1 == split_key_count ? std::string("") : std::string(split_keys[extra_id + 1])
                        );
                        LOG_INFO(
                            log,
                            "Add extra parallel prehandle task split_id={}/total={} limit {}",
                            extra_id,
                            split_keys.size(),
                            limit.toDebugString());
                        runInParallel(extra_id, ctx, std::move(limit), dm_storage);
                        return true;
                    });
                    RUNTIME_CHECK_MSG(
                        add_result,
                        "Failed when add {}-th task for prehandling region_id={}",
                        extra_id,
                        new_region->id());
                }
                LOG_INFO(
                    log,
                    "Add extra parallel prehandle task split_id={}/total={} limit {}",
                    DM::SSTScanSoftLimit::HEAD_SPLIT,
                    split_keys.size(),
                    sst_stream->getSoftLimit().value().toDebugString());
                auto [head_result, head_prehandle_result]
                    = executeTransform(new_region, prehandle_task, job_type, storage, sst_stream, opt, tmt);
                LOG_INFO(
                    log,
                    "Finished extra parallel prehandle task limit {} write cf {} dmfiles {} error {}, split_id={}, region_id={}",
                    sst_stream->getSoftLimit()->toDebugString(),
                    head_prehandle_result.stats.write_cf_keys,
                    head_prehandle_result.ingest_ids.size(),
                    magic_enum::enum_name(head_result.error),
                    DM::SSTScanSoftLimit::HEAD_SPLIT,
                    new_region->id());

                if (head_result.error == ReadFromStreamError::Ok)
                {
                    prehandle_result = std::move(head_prehandle_result);
                    // Wait all threads to join.
                    for (size_t extra_id = 0; extra_id < split_key_count; extra_id++)
                    {
                        // May get exception.
                        LOG_INFO(
                            log,
                            "Try fetch prehandle task split_id={}, region_id={}",
                            extra_id,
                            new_region->id());
                        async_tasks.fetchResult(extra_id);
                    }
                    // Aggregate results.
                    for (size_t extra_id = 0; extra_id < split_key_count; extra_id++)
                    {
                        std::scoped_lock l(ctx->mut);
                        if (ctx->gather_res[extra_id].error == ReadFromStreamError::Ok)
                        {
                            result.error = ReadFromStreamError::Ok;
                            auto & v = ctx->gather_prehandle_res[extra_id];
                            prehandle_result.ingest_ids.insert(
                                prehandle_result.ingest_ids.end(),
                                std::make_move_iterator(v.ingest_ids.begin()),
                                std::make_move_iterator(v.ingest_ids.end()));
                            v.ingest_ids.clear();
                            prehandle_result.stats.mergeFrom(v.stats);
                        }
                        else
                        {
                            // Once a prehandle has non-ok result, we quit further loop
                            result = ctx->gather_res[extra_id];
                            break;
                        }
                    }
                    LOG_INFO(
                        log,
                        "Finished all extra parallel prehandle task write cf {} dmfiles {} error {}, region_id={}",
                        prehandle_result.stats.write_cf_keys,
                        prehandle_result.ingest_ids.size(),
                        magic_enum::enum_name(head_result.error),
                        new_region->id());
                } else {
                    // Otherwise, fallback to error handling or exception handling.
                    result = head_result;
                }
            }

            if (result.error == ReadFromStreamError::ErrUpdateSchema)
            {
                // It will be thrown in `SSTFilesToBlockInputStream`.
                // The schema of decoding region data has been updated, need to clear and recreate another stream for writing DTFile(s)
                new_region->clearAllData();

                if (force_decode)
                {
                    // Can not decode data with `force_decode == true`, must be something wrong
                    throw Exception(result.extra_msg, ErrorCodes::REGION_DATA_SCHEMA_UPDATED);
                }

                // Update schema and try to decode again
                LOG_INFO(
                    log,
                    "Decoding Region snapshot data meet error, sync schema and try to decode again {} [error={}]",
                    new_region->toString(true),
                    result.extra_msg);
                GET_METRIC(tiflash_schema_trigger_count, type_raft_decode).Increment();
                tmt.getSchemaSyncerManager()->syncTableSchema(context, keyspace_id, physical_table_id);
                // Next time should force_decode
                force_decode = true;

                continue;
            }
            else if (result.error == ReadFromStreamError::ErrTableDropped)
            {
                // We can ignore if storage is dropped.
                LOG_INFO(
                    log,
                    "Pre-handle snapshot to DTFiles is ignored because the table is dropped {}",
                    new_region->toString(true));
                break;
            }
            else if (result.error == ReadFromStreamError::Aborted)
            {
                LOG_INFO(
                    log,
                    "Apply snapshot is aborted, cancelling. region_id={} term={} index={}",
                    region_id,
                    term,
                    index);
            }

            (void)table_drop_lock; // the table should not be dropped during ingesting file
            break;
        }
        catch (DB::Exception & e)
        {
            if (e.code() == ErrorCodes::TABLE_IS_DROPPED)
            {
                // It will be thrown in many places that will lock a table.
                // We can ignore if storage is dropped.
                LOG_INFO(
                    log,
                    "Pre-handle snapshot to DTFiles is ignored because the table is dropped {}",
                    new_region->toString(true));
                break;
            }
            else
            {
                // Other unrecoverable error, throw
                e.addMessage(fmt::format("keyspace={} physical_table_id={}", keyspace_id, physical_table_id));
                throw;
            }
        }
    } // while

    return prehandle_result;
}

void KVStore::abortPreHandleSnapshot(UInt64 region_id, TMTContext & tmt)
{
    UNUSED(tmt);
    auto cancel_flag = prehandling_trace.deregisterTask(region_id);
    if (cancel_flag)
    {
        // The task is registered, set the cancel flag to true and the generated files
        // will be clear later by `releasePreHandleSnapshot`
        LOG_INFO(log, "Try cancel pre-handling from upper layer, region_id={}", region_id);
        cancel_flag->store(true, std::memory_order_seq_cst);
    }
    else
    {
        // the task is not registered, continue
        LOG_INFO(log, "Start cancel pre-handling from upper layer, region_id={}", region_id);
    }
}

template <>
void KVStore::releasePreHandledSnapshot<RegionPtrWithSnapshotFiles>(
    const RegionPtrWithSnapshotFiles & s,
    TMTContext & tmt)
{
    auto & storages = tmt.getStorages();
    auto keyspace_id = s.base->getKeyspaceID();
    auto table_id = s.base->getMappedTableID();
    auto storage = storages.get(keyspace_id, table_id);
    if (storage->engineType() != TiDB::StorageEngine::DT)
    {
        return;
    }
    auto dm_storage = std::dynamic_pointer_cast<StorageDeltaMerge>(storage);
    LOG_INFO(
        log,
        "Release prehandled snapshot, clean {} dmfiles, region_id={} keyspace={} table_id={}",
        s.external_files.size(),
        s.base->id(),
        keyspace_id,
        table_id);
    auto & context = tmt.getContext();
    dm_storage->cleanPreIngestFiles(s.external_files, context.getSettingsRef());
}

void Region::beforePrehandleSnapshot(uint64_t region_id, std::optional<uint64_t> deadline_index)
{
    if (getClusterRaftstoreVer() == RaftstoreVer::V2)
    {
        data.orphan_keys_info.snapshot_index = appliedIndex();
        data.orphan_keys_info.pre_handling = true;
        data.orphan_keys_info.deadline_index = deadline_index;
        data.orphan_keys_info.region_id = region_id;
    }
}

void Region::afterPrehandleSnapshot(int64_t ongoing)
{
    if (getClusterRaftstoreVer() == RaftstoreVer::V2)
    {
        data.orphan_keys_info.pre_handling = false;
        LOG_INFO(
            log,
            "After prehandle, remains orphan keys {} removed orphan keys {} ongoing {} [region_id={}]",
            data.orphan_keys_info.remainedKeyCount(),
            data.orphan_keys_info.removed_remained_keys.size(),
            ongoing,
            id());
    }
}

} // namespace DB