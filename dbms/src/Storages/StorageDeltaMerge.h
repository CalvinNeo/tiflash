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

#include <Common/Logger.h>
#include <Common/UniThreadPool.h>
#include <Core/Defines.h>
#include <Core/SortDescription.h>
#include <Flash/Coprocessor/RuntimeFilterMgr.h>
#include <Storages/DeltaMerge/DMChecksumConfig.h>
#include <Storages/DeltaMerge/DeltaMergeDefines.h>
#include <Storages/DeltaMerge/DeltaMergeInterfaces.h>
#include <Storages/DeltaMerge/Filter/PushDownFilter.h>
#include <Storages/DeltaMerge/Remote/DisaggSnapshot_fwd.h>
#include <Storages/DeltaMerge/ScanContext.h>
#include <Storages/IManageableStorage.h>
#include <Storages/IStorage.h>
#include <Storages/KVStore/Decode/DecodingStorageSchemaSnapshot.h>
#include <TiDB/Schema/TiDB.h>

#include <ext/shared_ptr_helper.h>

namespace DB
{
struct CheckpointInfo;
using CheckpointInfoPtr = std::shared_ptr<CheckpointInfo>;
namespace DM
{
struct RowKeyRange;
struct RowKeyValue;
class DeltaMergeStore;
using DeltaMergeStorePtr = std::shared_ptr<DeltaMergeStore>;
using RowKeyRanges = std::vector<RowKeyRange>;
struct ExternalDTFileInfo;
struct GCOptions;
} // namespace DM

class StorageDeltaMerge
    : public ext::SharedPtrHelper<StorageDeltaMerge>
    , public IManageableStorage
{
public:
    ~StorageDeltaMerge() override;

    bool supportsModification() const override { return true; }

    String getName() const override;
    String getTableName() const override;
    String getDatabaseName() const override;

    void clearData() override;

    void drop() override;

    BlockInputStreams read(
        const Names & column_names,
        const SelectQueryInfo & query_info,
        const Context & context,
        QueryProcessingStage::Enum & processed_stage,
        size_t max_block_size,
        unsigned num_streams) override;

    void read(
        PipelineExecutorContext & exec_context_,
        PipelineExecGroupBuilder & group_builder,
        const Names & column_names,
        const SelectQueryInfo & query_info,
        const Context & context,
        size_t max_block_size,
        unsigned num_streams) override;

    DM::Remote::DisaggPhysicalTableReadSnapshotPtr writeNodeBuildRemoteReadSnapshot(
        const Names & column_names,
        const SelectQueryInfo & query_info,
        const Context & context,
        unsigned num_streams);

    BlockOutputStreamPtr write(const ASTPtr & query, const Settings & settings) override;

    /// Write from raft layer.
    DM::WriteResult write(Block & block, const Settings & settings);

    void flushCache(const Context & context) override;

    bool flushCache(const Context & context, const DM::RowKeyRange & range_to_flush, bool try_until_succeed) override;

    /// Merge delta into the stable layer for all segments.
    ///
    /// This function is called when using `MANAGE TABLE [TABLE] MERGE DELTA` from TiFlash Client.
    void mergeDelta(const Context & context);

    /// Merge delta into the stable layer for one segment located by the specified start key.
    /// Returns the range of the merged segment, which can be used to merge the remaining segments incrementally (new_start_key = old_end_key).
    /// If there is no segment found by the start key, nullopt is returned.
    ///
    /// This function is called when using `ALTER TABLE [TABLE] COMPACT ...` from TiDB.
    std::optional<DM::RowKeyRange> mergeDeltaBySegment(const Context & context, const DM::RowKeyValue & start_key);

    void deleteRange(const DM::RowKeyRange & range_to_delete, const Settings & settings);

    // If the "ingest" has been aborted, we use this method to
    // clean the generated external files on the fly.
    void cleanPreIngestFiles(const std::vector<DM::ExternalDTFileInfo> & external_files, const Settings & settings);

    /// Return the 'ingtested bytes'.
    UInt64 ingestFiles(
        const DM::RowKeyRange & range,
        const std::vector<DM::ExternalDTFileInfo> & external_files,
        bool clear_data_in_range,
        const Settings & settings);

    void ingestSegmentsFromCheckpointInfo(
        const DM::RowKeyRange & range,
        CheckpointInfoPtr checkpoint_info,
        const Settings & settings);

    UInt64 onSyncGc(Int64, const DM::GCOptions &) override;

    void rename(
        const String & new_path_to_db,
        const String & new_database_name,
        const String & new_table_name,
        const String & new_display_table_name) override;

    void modifyASTStorage(ASTStorage * storage_ast, const TiDB::TableInfo & table_info) override;

    void alter(
        const TableLockHolder &,
        const AlterCommands & commands,
        const String & database_name,
        const String & table_name,
        const Context & context) override;

    void updateTombstone(
        const TableLockHolder &,
        const AlterCommands & commands,
        const String & database_name,
        const TiDB::TableInfo & table_info,
        const SchemaNameMapper & name_mapper,
        const Context & context) override;

    void alterSchemaChange(
        const TableLockHolder &,
        TiDB::TableInfo & table_info,
        const String & database_name,
        const String & table_name,
        const Context & context) override;

    void setTableInfo(const TiDB::TableInfo & table_info_) override { tidb_table_info = table_info_; }

    ::TiDB::StorageEngine engineType() const override { return ::TiDB::StorageEngine::DT; }

    const TiDB::TableInfo & getTableInfo() const override { return tidb_table_info; }

    void startup() override;

    void shutdown() override;

    void removeFromTMTContext() override;

    SortDescription getPrimarySortDescription() const override;

    const OrderedNameSet & getHiddenColumnsImpl() const override { return hidden_columns; }

    BlockInputStreamPtr status() override;

    void checkStatus(const Context & context) override;
    void deleteRows(const Context &, size_t rows) override;

    const DM::DeltaMergeStorePtr & getStore() { return getAndMaybeInitStore(); }

    DM::DeltaMergeStorePtr getStoreIfInited() const;

    bool isCommonHandle() const override { return is_common_handle; }

    size_t getRowKeyColumnSize() const override { return rowkey_column_size; }

    std::pair<DB::DecodingStorageSchemaSnapshotConstPtr, BlockUPtr> getSchemaSnapshotAndBlockForDecoding(
        const TableStructureLockHolder & table_structure_lock,
        bool /* need_block */) override;

    void releaseDecodingBlock(Int64 block_decoding_schema_epoch, BlockUPtr block) override;

    bool initStoreIfDataDirExist(ThreadPool * thread_pool) override;

    DM::DMConfigurationOpt createChecksumConfig() const { return DM::DMChecksumConfig::fromDBContext(global_context); }

    static DM::PushDownFilterPtr buildPushDownFilter(
        const DM::RSOperatorPtr & rs_operator,
        const ColumnInfos & table_scan_column_info,
        const google::protobuf::RepeatedPtrField<tipb::Expr> & pushed_down_filters,
        const DM::ColumnDefines & columns_to_read,
        const Context & context,
        const LoggerPtr & tracing_logger);

#ifndef DBMS_PUBLIC_GTEST
protected:
#endif

    StorageDeltaMerge(
        const String & db_engine,
        const String & db_name_,
        const String & name_,
        DM::OptionTableInfoConstRef table_info_,
        const ColumnsDescription & columns_,
        const ASTPtr & primary_expr_ast_,
        Timestamp tombstone,
        Context & global_context_);

    Block buildInsertBlock(bool is_import, bool is_delete, const Block & block);

#ifndef DBMS_PUBLIC_GTEST
private:
#endif

    void alterImpl(
        const AlterCommands & commands,
        const String & database_name,
        const String & table_name,
        const Context & context);

    DataTypePtr getPKTypeImpl() const override;

    DM::DeltaMergeStorePtr & getAndMaybeInitStore(ThreadPool * thread_pool = nullptr);
    bool storeInited() const { return store_inited.load(std::memory_order_acquire); }
    void updateTableColumnInfo();
    ColumnsDescription getNewColumnsDescription(const TiDB::TableInfo & table_info);
    DM::ColumnDefines getStoreColumnDefines() const override;
    bool dataDirExist();
    void shutdownImpl();

    DM::RSOperatorPtr buildRSOperator(
        const std::unique_ptr<DAGQueryInfo> & dag_query,
        const DM::ColumnDefines & columns_to_read,
        const Context & context,
        const LoggerPtr & tracing_logger);
    /// Get filters from query to construct rough set operation and push down filters.
    DM::PushDownFilterPtr parsePushDownFilter(
        const SelectQueryInfo & query_info,
        const DM::ColumnDefines & columns_to_read,
        const Context & context,
        const LoggerPtr & tracing_logger);

    DM::RowKeyRanges parseMvccQueryInfo(
        const DB::MvccQueryInfo & mvcc_query_info,
        unsigned num_streams,
        const Context & context,
        const String & req_id,
        const LoggerPtr & tracing_logger);

    RuntimeFilteList parseRuntimeFilterList(const SelectQueryInfo & query_info, const Context & db_context) const;

#ifndef DBMS_PUBLIC_GTEST
private:
#endif

    struct TableColumnInfo
    {
        TableColumnInfo(const String & db, const String & table, const ASTPtr & pk)
            : db_name(db)
            , table_name(table)
            , pk_expr_ast(pk)
        {}

        String db_name;
        String table_name;
        ASTPtr pk_expr_ast;
        DM::ColumnDefines table_column_defines;
        DM::ColumnDefine handle_column_define;
    };
    const bool data_path_contains_database_name = false;

    mutable std::mutex store_mutex;

    std::unique_ptr<TableColumnInfo> table_column_info; // After create DeltaMergeStore object, it is deprecated.
    std::atomic<bool> store_inited;
    DM::DeltaMergeStorePtr _store; // NOLINT(readability-identifier-naming)

    Strings pk_column_names; // TODO: remove it. Only use for debug from ch-client.
    bool is_common_handle = false;
    bool pk_is_handle = false;
    size_t rowkey_column_size = 0;
    OrderedNameSet hidden_columns;

    // The table schema synced from TiDB
    TiDB::TableInfo tidb_table_info;

    mutable std::mutex decode_schema_mutex;
    DecodingStorageSchemaSnapshotPtr decoding_schema_snapshot;
    // The following two members must be used under the protection of table structure lock
    bool decoding_schema_changed = false;
    // internal epoch for `decoding_schema_snapshot`
    Int64 decoding_schema_epoch = 1;

    // avoid creating block every time when decoding row
    std::vector<BlockUPtr> cache_blocks;
    // avoid creating too many cached blocks(the typical num should be less and equal than raft apply thread)
    static constexpr size_t max_cached_blocks_num = 16;

    // Used to allocate new column-id when this table is NOT synced from TiDB
    ColumnID max_column_id_used;

    std::atomic<bool> shutdown_called{false};

    std::atomic<UInt64> next_version = 1; //TODO: remove this!!!

    Context & global_context;

    LoggerPtr log;
};


} // namespace DB
