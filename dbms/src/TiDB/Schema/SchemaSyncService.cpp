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

#include <Common/TiFlashMetrics.h>
#include <Databases/IDatabase.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterDropQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Storages/BackgroundProcessingPool.h>
#include <Storages/IManageableStorage.h>
#include <Storages/KVStore/TMTContext.h>
#include <Storages/KVStore/Types.h>
#include <TiDB/Schema/SchemaNameMapper.h>
#include <TiDB/Schema/SchemaSyncService.h>
#include <TiDB/Schema/SchemaSyncer.h>
#include <TiDB/Schema/TiDBSchemaManager.h>
#include <common/logger_useful.h>

namespace DB
{
namespace ErrorCodes
{
extern const int DEADLOCK_AVOIDED;
} // namespace ErrorCodes

SchemaSyncService::SchemaSyncService(DB::Context & context_)
    : context(context_)
    , background_pool(context_.getBackgroundPool())
    , log(Logger::get())
{
    // Add task for adding and removing keyspace sync schema tasks.
    handle = background_pool.addTask(
        [&, this] {
            addKeyspaceGCTasks();
            removeKeyspaceGCTasks();

            return false;
        },
        false,
        context.getSettingsRef().ddl_sync_interval_seconds * 1000);
}

void SchemaSyncService::addKeyspaceGCTasks()
{
    const auto keyspaces = context.getTMTContext().getStorages().getAllKeyspaces();
    std::unique_lock<std::shared_mutex> lock(keyspace_map_mutex);

    // Add new sync schema task for new keyspace.
    for (auto const iter : keyspaces)
    {
        auto keyspace = iter.first;
        if (keyspace_handle_map.contains(keyspace))
            continue;

        auto ks_log = log->getChild(fmt::format("keyspace={}", keyspace));
        LOG_INFO(ks_log, "add sync schema task");
        auto task_handle = background_pool.addTask(
            [&, this, keyspace, ks_log]() noexcept {
                String stage;
                bool done_anything = false;
                try
                {
                    /// Do sync schema first, then gc.
                    /// They must be performed synchronously,
                    /// otherwise table may get mis-GC-ed if RECOVER was not properly synced caused by schema sync pause but GC runs too aggressively.
                    // GC safe point must be obtained ahead of syncing schema.
                    auto gc_safe_point
                        = PDClientHelper::getGCSafePointWithRetry(context.getTMTContext().getPDClient(), keyspace);
                    stage = "Sync schemas";
                    done_anything = syncSchemas(keyspace);
                    if (done_anything)
                        GET_METRIC(tiflash_schema_trigger_count, type_timer).Increment();

                    stage = "GC";
                    done_anything = gc(gc_safe_point, keyspace);

                    return done_anything;
                }
                catch (const Exception & e)
                {
                    LOG_ERROR(
                        ks_log,
                        "{} failed by {} \n stack : {}",
                        stage,
                        e.displayText(),
                        e.getStackTrace().toString());
                }
                catch (const Poco::Exception & e)
                {
                    LOG_ERROR(ks_log, "{} failed by {}", stage, e.displayText());
                }
                catch (const std::exception & e)
                {
                    LOG_ERROR(ks_log, "{} failed by {}", stage, e.what());
                }
                return false;
            },
            false,
            context.getSettingsRef().ddl_sync_interval_seconds * 1000);

        keyspace_handle_map.emplace(keyspace, task_handle);
    }
}

void SchemaSyncService::removeKeyspaceGCTasks()
{
    const auto keyspaces = context.getTMTContext().getStorages().getAllKeyspaces();
    std::unique_lock<std::shared_mutex> lock(keyspace_map_mutex);

    // Remove stale sync schema task.
    for (auto keyspace_handle_iter = keyspace_handle_map.begin(); keyspace_handle_iter != keyspace_handle_map.end();
         /*empty*/)
    {
        const auto & keyspace = keyspace_handle_iter->first;
        if (keyspaces.count(keyspace))
        {
            ++keyspace_handle_iter;
            continue;
        }
        auto keyspace_log = log->getChild(fmt::format("keyspace={}", keyspace));
        LOG_INFO(keyspace_log, "remove sync schema task");
        background_pool.removeTask(keyspace_handle_iter->second);
        keyspace_handle_iter = keyspace_handle_map.erase(keyspace_handle_iter);

        context.getTMTContext().getSchemaSyncerManager()->removeSchemaSyncer(keyspace);
        PDClientHelper::remove_ks_gc_sp(keyspace);

        keyspace_gc_context.erase(keyspace);
    }
}

SchemaSyncService::~SchemaSyncService()
{
    if (handle)
    {
        // stop the root handle first
        background_pool.removeTask(handle);
    }

    for (auto const & iter : keyspace_handle_map)
    {
        auto task_handle = iter.second;
        background_pool.removeTask(task_handle);
    }
    LOG_INFO(log, "SchemaSyncService stopped");
}

bool SchemaSyncService::syncSchemas(KeyspaceID keyspace_id)
{
    return context.getTMTContext().getSchemaSyncerManager()->syncSchemas(context, keyspace_id);
}

template <typename DatabaseOrTablePtr>
inline std::tuple<bool, Timestamp> isSafeForGC(const DatabaseOrTablePtr & ptr, Timestamp gc_safepoint)
{
    const auto tombstone_ts = ptr->getTombstone();
    return {tombstone_ts != 0 && tombstone_ts < gc_safepoint, tombstone_ts};
}

Timestamp SchemaSyncService::lastGcSafePoint(KeyspaceID keyspace_id) const
{
    std::shared_lock lock(keyspace_map_mutex);
    auto iter = keyspace_gc_context.find(keyspace_id);
    if (iter == keyspace_gc_context.end())
        return 0;
    return iter->second.last_gc_safepoint;
}

void SchemaSyncService::updateLastGcSafepoint(KeyspaceID keyspace_id, Timestamp gc_safepoint)
{
    std::unique_lock lock(keyspace_map_mutex);
    keyspace_gc_context[keyspace_id].last_gc_safepoint = gc_safepoint;
}

bool SchemaSyncService::gc(Timestamp gc_safepoint, KeyspaceID keyspace_id)
{
    const Timestamp last_gc_safepoint = lastGcSafePoint(keyspace_id);
    if (last_gc_safepoint != 0 && gc_safepoint == last_gc_safepoint)
        return false;

    auto keyspace_log = log->getChild(fmt::format("keyspace={}", keyspace_id));
    LOG_INFO(keyspace_log, "Schema GC begin, last_safepoint={} safepoint={}", last_gc_safepoint, gc_safepoint);

    size_t num_tables_removed = 0;
    size_t num_databases_removed = 0;

    auto & tmt_context = context.getTMTContext();
    // The storages that are ready for gc
    std::vector<std::weak_ptr<IManageableStorage>> storages_to_gc;
    // Get a snapshot of database
    auto dbs = context.getDatabases();
    for (const auto & iter : dbs)
    {
        auto db_keyspace_id = SchemaNameMapper::getMappedNameKeyspaceID(iter.first);
        if (db_keyspace_id != keyspace_id)
            continue;
        const auto & db = iter.second;
        for (auto table_iter = db->getIterator(context); table_iter->isValid(); table_iter->next())
        {
            auto & storage = table_iter->table();
            auto managed_storage = std::dynamic_pointer_cast<IManageableStorage>(storage);
            if (!managed_storage)
                continue;

            const auto & [database_is_stale, db_tombstone] = isSafeForGC(db, gc_safepoint);
            const auto & [table_is_stale, table_tombstone] = isSafeForGC(managed_storage, gc_safepoint);
            if (database_is_stale || table_is_stale)
            {
                // Only keep a weak_ptr on storage so that the memory can be free as soon as
                // it is dropped.
                storages_to_gc.emplace_back(std::weak_ptr<IManageableStorage>(managed_storage));
                LOG_INFO(
                    log,
                    "Detect stale table, database_name={} table_name={} database_tombstone={} table_tombstone={} "
                    "safepoint={}",
                    managed_storage->getDatabaseName(),
                    managed_storage->getTableName(),
                    db_tombstone,
                    table_tombstone,
                    gc_safepoint);
            }
        }
    }

    // Physically drop tables
    bool succeeded = true;
    for (auto & storage_ptr : storages_to_gc)
    {
        // Get a shared_ptr from weak_ptr, it should always success.
        auto storage = storage_ptr.lock();
        if (unlikely(!storage))
            continue;

        String database_name = storage->getDatabaseName();
        String table_name = storage->getTableName();
        const auto & table_info = storage->getTableInfo();

        tmt_context.getSchemaSyncerManager()->removeTableID(keyspace_id, table_info.id);

        auto canonical_name = [&]() {
            // DB info maintenance is parallel with GC logic so we can't always assume one specific DB info's existence, thus checking its validity.
            auto db_info = tmt_context.getSchemaSyncerManager()->getDBInfoByMappedName(keyspace_id, database_name);
            return db_info ? fmt::format(
                       "{}, database_id={} table_id={}",
                       SchemaNameMapper().debugCanonicalName(*db_info, table_info),
                       db_info->id,
                       table_info.id)
                           : fmt::format(
                               "({}).{}, table_id={}",
                               database_name,
                               SchemaNameMapper().debugTableName(table_info),
                               table_info.id);
        }();
        LOG_INFO(
            keyspace_log,
            "Physically dropping table, table_tombstone={} safepoint={} {}",
            storage->getTombstone(),
            gc_safepoint,
            canonical_name);
        auto drop_query = std::make_shared<ASTDropQuery>();
        drop_query->database = std::move(database_name);
        drop_query->table = std::move(table_name);
        drop_query->if_exists = true;
        drop_query->lock_timeout = std::chrono::milliseconds(1 * 1000); // timeout for acquring table drop lock
        ASTPtr ast_drop_query = drop_query;
        try
        {
            InterpreterDropQuery drop_interpreter(ast_drop_query, context);
            drop_interpreter.execute();
            LOG_INFO(keyspace_log, "Physically dropped table {}", canonical_name);
            ++num_tables_removed;
        }
        catch (DB::Exception & e)
        {
            succeeded = false;
            String err_msg;
            // Maybe a read lock of a table is held for a long time, just ignore it this round.
            if (e.code() == ErrorCodes::DEADLOCK_AVOIDED)
                err_msg = "locking attempt has timed out!"; // ignore verbose stack for this error
            else
                err_msg = getCurrentExceptionMessage(true);
            LOG_INFO(keyspace_log, "Physically drop table {} is skipped, reason: {}", canonical_name, err_msg);
        }
    }
    storages_to_gc.clear();

    // Physically drop database
    for (const auto & iter : dbs)
    {
        const auto & db = iter.second;
        auto ks_db_id = SchemaNameMapper::getMappedNameKeyspaceID(iter.first);
        if (ks_db_id != keyspace_id)
            continue;
        const auto & [db_is_stale, db_tombstone] = isSafeForGC(db, gc_safepoint);
        if (!db_is_stale)
            continue;

        const auto & db_name = iter.first;
        size_t num_tables = 0;
        for (auto table_iter = db->getIterator(context); table_iter->isValid(); table_iter->next())
            ++num_tables;
        if (num_tables > 0)
        {
            // There should be something wrong, maybe a read lock of a table is held for a long time.
            // Just ignore and try to collect this database next time.
            LOG_INFO(
                keyspace_log,
                "Physically drop database {} is skipped, reason: {} tables left",
                db_name,
                num_tables);
            continue;
        }

        LOG_INFO(keyspace_log, "Physically dropping database, database_tombstone={} {}", db->getTombstone(), db_name);
        auto drop_query = std::make_shared<ASTDropQuery>();
        drop_query->database = db_name;
        drop_query->if_exists = true;
        drop_query->lock_timeout = std::chrono::milliseconds(1 * 1000); // timeout for acquring table drop lock
        ASTPtr ast_drop_query = drop_query;
        try
        {
            InterpreterDropQuery drop_interpreter(ast_drop_query, context);
            drop_interpreter.execute();
            LOG_INFO(keyspace_log, "Physically dropped database {}, safepoint={}", db_name, gc_safepoint);
            ++num_databases_removed;
        }
        catch (DB::Exception & e)
        {
            succeeded = false;
            String err_msg;
            if (e.code() == ErrorCodes::DEADLOCK_AVOIDED)
                err_msg = "locking attempt has timed out!"; // ignore verbose stack for this error
            else
                err_msg = getCurrentExceptionMessage(true);
            LOG_INFO(keyspace_log, "Physically drop database {} is skipped, reason: {}", db_name, err_msg);
        }
    }

    if (succeeded)
    {
        updateLastGcSafepoint(keyspace_id, gc_safepoint);
        LOG_INFO(
            keyspace_log,
            "Schema GC done, tables_removed={} databases_removed={} safepoint={}",
            num_tables_removed,
            num_databases_removed,
            gc_safepoint);
    }
    else
    {
        // Don't update last_gc_safe_point and retry later
        LOG_INFO(
            keyspace_log,
            "Schema GC meet error, will try again later, last_safepoint={} safepoint={}",
            last_gc_safepoint,
            gc_safepoint);
    }

    return true;
}

} // namespace DB
