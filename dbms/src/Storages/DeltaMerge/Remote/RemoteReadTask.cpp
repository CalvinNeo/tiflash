#include <Common/Exception.h>
#include <Common/Logger.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/NullBlockInputStream.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadBufferFromString.h>
#include <Storages/DeltaMerge/DeltaMergeDefines.h>
#include <Storages/DeltaMerge/DeltaMergeHelpers.h>
#include <Storages/DeltaMerge/File/dtpb/column_file.pb.h>
#include <Storages/DeltaMerge/Filter/RSOperator.h>
#include <Storages/DeltaMerge/Remote/DisaggregatedTaskId.h>
#include <Storages/DeltaMerge/Remote/ObjectId.h>
#include <Storages/DeltaMerge/Remote/RemoteReadTask.h>
#include <Storages/DeltaMerge/Remote/RemoteSegmentThreadInputStream.h>
#include <Storages/DeltaMerge/RowKeyRange.h>
#include <Storages/DeltaMerge/Segment.h>
#include <Storages/Page/Page.h>
#include <Storages/Page/PageDefines.h>
#include <Storages/Transaction/Types.h>
#include <common/defines.h>
#include <common/logger_useful.h>
#include <common/types.h>

#include <memory>
#include <mutex>

namespace DB::DM
{

RemoteReadTask::RemoteReadTask(std::vector<RemoteTableReadTaskPtr> && tasks_)
    : num_segments(0)
{
    for (const auto & table_task : tasks_)
    {
        if (!table_task)
            continue;
        auto res = tasks.emplace(table_task->storeID(), table_task);
        RUNTIME_CHECK_MSG(res.second, "Duplicated task from store_id={}", table_task->storeID());
        num_segments += table_task->size();

        // Push all inited tasks to ready queue
        for (const auto & task : table_task->allTasks())
        {
            // TODO: If all pages are ready in local
            // cache, and the segment does not contains any
            // blocks on write node's mem-table, then we
            // can simply skip the fetch page pharse and
            // push it into ready queue

            if (auto iter = ready_segment_tasks.find(task->state); iter != ready_segment_tasks.end())
            {
                iter->second.push_back(task);
            }
            else
            {
                ready_segment_tasks.emplace(task->state, std::list<RemoteSegmentReadTaskPtr>{task});
            }
        }
    }
    curr_store = tasks.begin();
}

size_t RemoteReadTask::numSegments() const
{
    return num_segments;
}

RemoteSegmentReadTaskPtr RemoteReadTask::nextFetchTask()
{
    // A simple scheduling policy that try to execute the segment tasks
    // from different stores in parallel
    std::lock_guard gurad(mtx_tasks);
    while (true)
    {
        if (tasks.empty())
            return nullptr;

        if (curr_store->second->size() > 0)
        {
            auto task = curr_store->second->nextTask();
            // Move to next store
            curr_store++;
            if (curr_store == tasks.end())
                curr_store = tasks.begin();
            return task;
        }
        // No tasks left in this store, erase and try to pop task from the next store
        curr_store = tasks.erase(curr_store);
        if (curr_store == tasks.end())
            curr_store = tasks.begin();
    }
}

void RemoteReadTask::updateTaskState(const RemoteSegmentReadTaskPtr & seg_task, bool meet_error)
{
    {
        std::unique_lock ready_lock(mtx_ready_tasks);
        const auto old_state = seg_task->state;
        auto state_iter = ready_segment_tasks.find(old_state);
        RUNTIME_CHECK(state_iter != ready_segment_tasks.end());

        // TODO: make it an unordered_map
        bool found = false;
        for (auto task_iter = state_iter->second.begin(); task_iter != state_iter->second.end(); task_iter++)
        {
            auto & task = *task_iter;
            if (!(task->store_id == seg_task->store_id
                  && task->table_id == seg_task->table_id
                  && task->segment_id == seg_task->segment_id))
            {
                continue;
            }
            seg_task->state = meet_error ? SegmentReadTaskState::Error : SegmentReadTaskState::AllReady;
            found = true;
            // Move it into the right ready state, note `task`/`task_iter` is invalid
            state_iter->second.erase(task_iter);
            if (state_iter->second.empty())
                ready_segment_tasks.erase(state_iter);

            insertReadyTask(seg_task, ready_lock);
            break;
        }
        RUNTIME_CHECK(found);
    }

    cv_ready_tasks.notify_one();
}

void RemoteReadTask::insertReadyTask(const RemoteSegmentReadTaskPtr & seg_task, std::unique_lock<std::mutex> &)
{
    if (auto state_iter = ready_segment_tasks.find(seg_task->state);
        state_iter != ready_segment_tasks.end())
        state_iter->second.push_back(seg_task);
    else
        ready_segment_tasks.emplace(seg_task->state, std::list<RemoteSegmentReadTaskPtr>{seg_task});
}

RemoteSegmentReadTaskPtr RemoteReadTask::nextReadyTask()
{
    std::unique_lock ready_lock(mtx_ready_tasks);
    RemoteSegmentReadTaskPtr seg_task = nullptr;
    cv_ready_tasks.wait(ready_lock, [this, &seg_task] {
        // All segment task are processed, return a nullptr
        if (doneOrErrorHappen())
            return true;

        // Check whether there are segment task ready for reading
        if (auto iter = ready_segment_tasks.find(SegmentReadTaskState::AllReady); iter != ready_segment_tasks.end())
        {
            if (iter->second.empty())
                return false;
            seg_task = iter->second.front();
            iter->second.pop_front();
            if (iter->second.empty())
            {
                ready_segment_tasks.erase(iter);
            }
            return true;
        }
        return false;
    });

    return seg_task;
}

bool RemoteReadTask::doneOrErrorHappen() const
{
    // All finished
    if (ready_segment_tasks.empty())
        return true;
    auto iter = ready_segment_tasks.find(SegmentReadTaskState::Error);
    if (iter != ready_segment_tasks.end() && !iter->second.empty())
    {
        // some tasks run into error when fetching pages
        return true; // NOLINT(readability-simplify-boolean-expr)
    }
    return false;
}

RemoteTableReadTaskPtr RemoteTableReadTask::buildFrom(
    const Context & db_context,
    const UInt64 store_id,
    const String & address,
    const DisaggregatedTaskId & snapshot_id,
    const dtpb::DisaggregatedPhysicalTable & remote_table,
    const LoggerPtr & log)
{
    // Deserialize from `DisaggregatedPhysicalTable`, this should also
    // ensure the local cache pages.
    auto table_task = std::make_shared<RemoteTableReadTask>(
        store_id,
        remote_table.table_id(),
        snapshot_id,
        address);
    for (const auto & remote_seg : remote_table.segments())
    {
        auto seg_task = RemoteSegmentReadTask::buildFrom(
            db_context,
            remote_seg,
            snapshot_id,
            table_task->store_id,
            table_task->table_id,
            table_task->address,
            log);
        table_task->tasks.emplace_back(std::move(seg_task));
    }
    return table_task;
}

/**
 * Remote segment
 */

Allocator<false> RemoteSegmentReadTask::allocator;

RemoteSegmentReadTask::RemoteSegmentReadTask(
    DisaggregatedTaskId snapshot_id_,
    UInt64 store_id_,
    TableID table_id_,
    UInt64 segment_id_,
    String address_)
    : snapshot_id(std::move(snapshot_id_))
    , store_id(store_id_)
    , table_id(table_id_)
    , segment_id(segment_id_)
    , address(std::move(address_))
{
}

RemoteSegmentReadTaskPtr RemoteSegmentReadTask::buildFrom(
    const Context & db_context,
    const dtpb::DisaggregatedSegment & proto,
    const DisaggregatedTaskId & snapshot_id,
    UInt64 store_id,
    TableID table_id,
    const String & address,
    const LoggerPtr & log)
{
    RowKeyRange segment_range;
    {
        ReadBufferFromString rb(proto.key_range());
        segment_range = RowKeyRange::deserialize(rb);
    }
    RowKeyRanges read_ranges(proto.read_key_ranges_size());
    for (int i = 0; i < proto.read_key_ranges_size(); ++i)
    {
        ReadBufferFromString rb(proto.read_key_ranges(i));
        read_ranges[i] = RowKeyRange::deserialize(rb);
    }

    auto task = std::make_shared<RemoteSegmentReadTask>(
        snapshot_id,
        store_id,
        table_id,
        proto.segment_id(),
        address);

    task->page_cache = db_context.getDMRemoteManager()->getPageCache();
    task->segment = std::make_shared<Segment>(
        log,
        /*epoch*/ 0,
        segment_range,
        proto.segment_id(),
        /*next_segment_id*/ 0,
        nullptr,
        nullptr);
    task->read_ranges = std::move(read_ranges);

    task->segment_snap = SegmentSnapshot::deserializeFromRemoteProtocol(
        db_context,
        store_id,
        table_id,
        proto);

    LOG_DEBUG(log, "ColumnFiles in MemTable: {}", task->segment_snap->delta->getMemTableSetSnapshot()->getColumnFileCount());
    LOG_DEBUG(log, "ColumnFiles in Persisted: {}", task->segment_snap->delta->getPersistedFileSetSnapshot()->getColumnFileCount());

    {
        auto persisted_cfs = task->segment_snap->delta->getPersistedFileSetSnapshot();
        std::vector<Remote::PageOID> all_persisted_ids;
        all_persisted_ids.reserve(persisted_cfs->getColumnFileCount());
        for (const auto & cfs : persisted_cfs->getColumnFiles())
        {
            if (auto * tiny = cfs->tryToTinyFile(); tiny)
            {
                auto page_oid = Remote::PageOID{
                    .write_node_id = store_id,
                    .table_id = table_id,
                    .page_id = tiny->getDataPageId(),
                };
                all_persisted_ids.emplace_back(page_oid);
            }
        }

        auto pending_oids = task->page_cache->getPendingIds(all_persisted_ids);
        task->pending_page_ids.reserve(pending_oids.size());
        for (const auto & oid : pending_oids)
        {
            task->pending_page_ids.emplace_back(oid.page_id);
        }
        LOG_INFO(log,
                 "local cache hit rate: {}, pending_ids: {}",
                 (all_persisted_ids.empty() ? "N/A" : fmt::format("{:.2f}%", 100.0 * pending_oids.size() / all_persisted_ids.size())),
                 task->pendingPageIds());
    }

    task->dm_context = std::make_shared<DMContext>(
        db_context,
        /* path_pool */ nullptr,
        /* storage_pool */ nullptr,
        /* min_version */ 0,
        /* is_common_handle */ segment_range.is_common_handle,
        /* rowkey_column_size */ segment_range.rowkey_column_size,
        db_context.getSettingsRef(),
        /* table_id */ table_id,
        /* scan_context */ std::make_shared<ScanContext>() // Currently we don't access its content
    );

    return task;
}

void RemoteSegmentReadTask::receivePage(dtpb::RemotePage && remote_page)
{
    std::lock_guard lock(mtx_queue);
    const size_t buf_size = remote_page.data().size();
#if 0
    char * data_buf = static_cast<char *>(allocator.alloc(buf_size));
    MemHolder mem_holder = createMemHolder(data_buf, [buf_size](char * p) {
        allocator.free(p, buf_size);
    });
    Page page;
    page.data = ByteBuffer(data_buf, data_buf + buf_size);
    page.mem_holder = mem_holder;
    persisted_pages.push({remote_page.page_id(), std::move(page)});
#endif

    // Use LocalPageCache
    auto oid = Remote::PageOID{
        .write_node_id = store_id,
        .table_id = table_id,
        .page_id = remote_page.page_id()};
    auto read_buffer = std::make_shared<ReadBufferFromMemory>(remote_page.data().data(), buf_size);
    PageFieldSizes field_sizes;
    field_sizes.reserve(remote_page.field_offsets_size());
    for (const auto & field_off : remote_page.field_offsets())
    {
        field_sizes.emplace_back(field_off);
    }
    page_cache->write(oid, std::move(read_buffer), buf_size, std::move(field_sizes));
    LOG_DEBUG(Logger::get(), "receive page, page_id={}", remote_page.page_id());
}

BlockInputStreamPtr RemoteSegmentReadTask::getInputStream(
    const ColumnDefines & columns_to_read,
    const RowKeyRanges & key_ranges,
    UInt64 read_tso,
    const DM::RSOperatorPtr & rs_filter,
    size_t expected_block_size)
{
    return segment->getInputStreamModeNormal(
        *dm_context,
        columns_to_read,
        segment_snap,
        key_ranges,
        rs_filter,
        read_tso,
        expected_block_size);
}

} // namespace DB::DM
