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

#include <Storages/DeltaMerge/ColumnFile/ColumnFileBig.h>
#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/Remote/Manager.h>
#include <Storages/DeltaMerge/RowKeyFilter.h>
#include <Storages/DeltaMerge/convertColumnTypeHelpers.h>
#include <Storages/PathPool.h>
#include <Storages/Page/universal/Readers.h>
#include <Storages/Transaction/TMTContext.h>
#include <Storages/Transaction/KVStore.h>
#include <Storages/Page/V3/Remote/CheckpointPageManager.h>
#include <Storages/DeltaMerge/File/DMFileWriterRemote.h>

namespace DB
{
namespace DM
{
ColumnFileBig::ColumnFileBig(const DMContext & context, const DMFilePtr & file_, const RowKeyRange & segment_range_)
    : file(file_)
    , segment_range(segment_range_)
{
    calculateStat(context);
}

void ColumnFileBig::calculateStat(const DMContext & context)
{
    auto index_cache = context.db_context.getGlobalContext().getMinMaxIndexCache();

    auto pack_filter = DMFilePackFilter::loadFrom(
        file,
        index_cache,
        /*set_cache_if_miss*/ false,
        {segment_range},
        EMPTY_FILTER,
        {},
        context.db_context.getFileProvider(),
        context.getReadLimiter(),
        context.scan_context,
        /*tracing_id*/ context.tracing_id);

    std::tie(valid_rows, valid_bytes) = pack_filter.validRowsAndBytes();
}

ColumnFileReaderPtr ColumnFileBig::getReader(
    const DMContext & context,
    const IColumnFileSetStorageReaderPtr &,
    const ColumnDefinesPtr & col_defs) const
{
    if (file->isRemote())
    {
        if (!remote_dm_file)
        {
            auto & db_context = context.db_context;
            UInt64 store_id = db_context.getTMTContext().getKVStore()->getStoreMeta().id();
            UInt64 file_id = file->fileId();
            const auto & remote_manager = db_context.getDMRemoteManager();
            auto data_store = remote_manager->getDataStore();
            auto self_oid = Remote::DMFileOID{
                .write_node_id = store_id,
                .table_id = context.table_id,
                .file_id = file_id,
            };
            auto prepared = data_store->prepareDMFile(self_oid);
            auto new_dmfile = prepared->restore(DMFile::ReadMetaMode::all());
            remote_dm_file = std::make_shared<ColumnFileBig>(context, new_dmfile, segment_range);
        }
        return std::make_shared<ColumnFileBigReader>(context, *remote_dm_file, col_defs);
    }
    else
    {
        return std::make_shared<ColumnFileBigReader>(context, *this, col_defs);
    }
}

void ColumnFileBig::serializeMetadata(WriteBuffer & buf, bool /*save_schema*/) const
{
    writeIntBinary(file->pageId(), buf);
    writeIntBinary(valid_rows, buf);
    writeIntBinary(valid_bytes, buf);
}

ColumnFilePersistedPtr ColumnFileBig::deserializeMetadata(DMContext & context, //
                                                          const RowKeyRange & segment_range,
                                                          ReadBuffer & buf)
{
    UInt64 file_page_id;
    size_t valid_rows, valid_bytes;

    readIntBinary(file_page_id, buf);
    readIntBinary(valid_rows, buf);
    readIntBinary(valid_bytes, buf);

    auto file_id = context.storage_pool->dataReader()->getNormalPageId(file_page_id);
    auto file_parent_path = context.path_pool->getStableDiskDelegator().getDTFilePath(file_id);

    auto dmfile = DMFile::restore(context.db_context.getFileProvider(), file_id, file_page_id, file_parent_path, DMFile::ReadMetaMode::all());

    auto * dp_file = new ColumnFileBig(dmfile, valid_rows, valid_bytes, segment_range);
    return std::shared_ptr<ColumnFileBig>(dp_file);
}

 ColumnFilePersistedPtr ColumnFileBig::deserializeMetadataFromRemote(DMContext & context, //
                                                            const RowKeyRange & target_range,
                                                            ReadBuffer & buf,
                                                                    UniversalPageStoragePtr temp_ps,
                                                            UInt64 checkpoint_store_id,
                                                            TableID ns_id,
                                                            WriteBatches & wbs)
 {
     UInt64 file_page_id;
     size_t valid_rows, valid_bytes;

     readIntBinary(file_page_id, buf);
     readIntBinary(valid_rows, buf);
     readIntBinary(valid_bytes, buf);

     auto remote_file_page_id = StorageReader::toFullUniversalPageId(getStoragePrefix(TableStorageTag::Data), ns_id, file_page_id);
     auto remote_orig_file_page_id = temp_ps->getNormalPageId(remote_file_page_id);
     auto remote_file_id = PS::V3::universal::ExternalIdTrait::getU64ID(remote_orig_file_page_id);
     auto delegator = context.path_pool->getStableDiskDelegator();
     auto new_file_id = context.storage_pool->newDataPageIdForDTFile(delegator, __PRETTY_FUNCTION__);
     const auto & db_context = context.db_context;
     wbs.data.putExternal(new_file_id, 0);
     if (const auto & remote_manager = db_context.getDMRemoteManager(); remote_manager != nullptr)
     {
         // 1. link remote file
         auto remote_oid = Remote::DMFileOID{
             .write_node_id = checkpoint_store_id,
             .table_id = ns_id,
             .file_id = remote_file_id,
         };
         auto & tmt = db_context.getTMTContext();
         UInt64 store_id = tmt.getKVStore()->getStoreMeta().id();
         auto self_oid = Remote::DMFileOID{
             .write_node_id = store_id,
             .table_id = ns_id,
             .file_id = new_file_id,
         };
         auto data_store = remote_manager->getDataStore();
         data_store->linkDMFile(remote_oid, self_oid);

         // 2. create a local file with only needed metadata
         auto parent_path = delegator.choosePath();
         auto new_dmfile = DMFile::create(new_file_id, parent_path, false, context.createChecksumConfig(false));
         new_dmfile->setRemote();
         DMFileWriterRemote remote_writer(new_dmfile, context.db_context.getFileProvider(), self_oid, data_store);
         remote_writer.write();
         remote_writer.finalize();
         // TODO: bytes on disk is not correct
         delegator.addDTFile(new_file_id, new_dmfile->getBytesOnDisk(), parent_path);
         wbs.writeLogAndData();
         new_dmfile->enableGC();
         auto * dp_file = new ColumnFileBig(new_dmfile, valid_rows, valid_bytes, target_range);
         return std::shared_ptr<ColumnFileBig>(dp_file);
     }
     else
     {
         RUNTIME_CHECK_MSG(false, "Shouldn't reach here");
     }
 }

std::shared_ptr<ColumnFileBig> ColumnFileBig::deserializeFromRemoteProtocol(
    const dtpb::ColumnFileBig & proto,
    const Remote::DMFileOID & oid,
    const Remote::IDataStorePtr & data_store,
    const RowKeyRange & segment_range)
{
    RUNTIME_CHECK(proto.file_id() == oid.file_id);
    LOG_DEBUG(Logger::get(), "Rebuild local ColumnFileBig from remote, dmf_oid={}", oid.info());

    auto prepared = data_store->prepareDMFile(oid);
    auto dmfile = prepared->restore(DMFile::ReadMetaMode::all());
    return std::make_shared<ColumnFileBig>(dmfile, proto.valid_rows(), proto.valid_bytes(), segment_range);
}

void ColumnFileBigReader::initStream()
{
    if (file_stream)
        return;

    DMFileBlockInputStreamBuilder builder(context.db_context);
    file_stream = builder
                      .setTracingID(context.tracing_id)
                      .build(column_file.getFile(), *col_defs, RowKeyRanges{column_file.segment_range}, context.scan_context);

    header = file_stream->getHeader();
    // If we only need to read pk and version columns, then cache columns data in memory.
    if (pk_ver_only)
    {
        Block block;
        size_t total_rows = 0;
        while ((block = file_stream->read()))
        {
            Columns columns;
            columns.push_back(block.getByPosition(0).column);
            if (col_defs->size() == 2)
                columns.push_back(block.getByPosition(1).column);
            cached_pk_ver_columns.push_back(std::move(columns));
            total_rows += block.rows();
            cached_block_rows_end.push_back(total_rows);
        }
    }
}

size_t ColumnFileBigReader::readRowsRepeatedly(MutableColumns & output_cols, size_t rows_offset, size_t rows_limit, const RowKeyRange * range)
{
    if (unlikely(rows_offset + rows_limit > column_file.valid_rows))
        throw Exception("Try to read more rows", ErrorCodes::LOGICAL_ERROR);

    /// Read pk and version columns from cached.

    auto [start_block_index, rows_start_in_start_block] = locatePosByAccumulation(cached_block_rows_end, rows_offset);
    auto [end_block_index, rows_end_in_end_block] = locatePosByAccumulation(cached_block_rows_end, //
                                                                            rows_offset + rows_limit);

    size_t actual_read = 0;
    for (size_t block_index = start_block_index; block_index < cached_pk_ver_columns.size() && block_index <= end_block_index;
         ++block_index)
    {
        size_t rows_start_in_block = block_index == start_block_index ? rows_start_in_start_block : 0;
        size_t rows_end_in_block
            = block_index == end_block_index ? rows_end_in_end_block : cached_pk_ver_columns[block_index].at(0)->size();
        size_t rows_in_block_limit = rows_end_in_block - rows_start_in_block;

        // Nothing to read.
        if (rows_start_in_block == rows_end_in_block)
            continue;

        const auto & columns = cached_pk_ver_columns.at(block_index);
        const auto & pk_column = columns[0];

        actual_read += copyColumnsData(columns, pk_column, output_cols, rows_start_in_block, rows_in_block_limit, range);
    }
    return actual_read;
}

size_t ColumnFileBigReader::readRowsOnce(MutableColumns & output_cols, //
                                         size_t rows_offset,
                                         size_t rows_limit,
                                         const RowKeyRange * range)
{
    auto read_next_block = [&, this]() -> bool {
        rows_before_cur_block += (static_cast<bool>(cur_block)) ? cur_block.rows() : 0;
        cur_block_data.clear();

        cur_block = file_stream->read();
        cur_block_offset = 0;

        if (!cur_block)
        {
            file_stream = {};
            return false;
        }
        else
        {
            for (size_t col_index = 0; col_index < output_cols.size(); ++col_index)
                cur_block_data.push_back(cur_block.getByPosition(col_index).column);
            return true;
        }
    };

    size_t rows_end = rows_offset + rows_limit;
    size_t actual_read = 0;
    size_t read_offset = rows_offset;
    while (read_offset < rows_end)
    {
        if (!cur_block || cur_block_offset == cur_block.rows())
        {
            if (unlikely(!read_next_block()))
                throw Exception("Not enough delta data to read [offset=" + DB::toString(rows_offset)
                                    + "] [limit=" + DB::toString(rows_limit) + "] [read_offset=" + DB::toString(read_offset) + "]",
                                ErrorCodes::LOGICAL_ERROR);
        }
        if (unlikely(read_offset < rows_before_cur_block + cur_block_offset))
            throw Exception("read_offset is too small [offset=" + DB::toString(rows_offset) + "] [limit=" + DB::toString(rows_limit)
                                + "] [read_offset=" + DB::toString(read_offset)
                                + "] [min_offset=" + DB::toString(rows_before_cur_block + cur_block_offset) + "]",
                            ErrorCodes::LOGICAL_ERROR);

        if (read_offset >= rows_before_cur_block + cur_block.rows())
        {
            cur_block_offset = cur_block.rows();
            continue;
        }
        auto read_end_for_cur_block = std::min(rows_end, rows_before_cur_block + cur_block.rows());

        auto read_start_in_block = read_offset - rows_before_cur_block;
        auto read_limit_in_block = read_end_for_cur_block - read_offset;

        actual_read += copyColumnsData(cur_block_data, cur_block_data[0], output_cols, read_start_in_block, read_limit_in_block, range);
        read_offset += read_limit_in_block;
        cur_block_offset += read_limit_in_block;
    }
    return actual_read;
}

size_t ColumnFileBigReader::readRows(MutableColumns & output_cols, size_t rows_offset, size_t rows_limit, const RowKeyRange * range)
{
    initStream();

    try
    {
        if (pk_ver_only)
            return readRowsRepeatedly(output_cols, rows_offset, rows_limit, range);
        else
            return readRowsOnce(output_cols, rows_offset, rows_limit, range);
    }
    catch (DB::Exception & e)
    {
        e.addMessage(" while reading DTFile " + column_file.getFile()->path());
        throw;
    }
}

Block ColumnFileBigReader::readNextBlock()
{
    initStream();

    if (pk_ver_only)
    {
        if (next_block_index_in_cache >= cached_pk_ver_columns.size())
        {
            return {};
        }
        auto & columns = cached_pk_ver_columns[next_block_index_in_cache];
        next_block_index_in_cache += 1;
        return header.cloneWithColumns(std::move(columns));
    }
    else
    {
        return file_stream->read();
    }
}

ColumnFileReaderPtr ColumnFileBigReader::createNewReader(const ColumnDefinesPtr & new_col_defs)
{
    // Currently we don't reuse the cache data.
    return std::make_shared<ColumnFileBigReader>(context, column_file, new_col_defs);
}

} // namespace DM
} // namespace DB
