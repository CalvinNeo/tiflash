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

#include <Common/Exception.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/DeltaMerge/DeltaMergeDefines.h>
#include <Storages/DeltaMerge/File/DMFile.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/File/DMFileIndexWriter.h>
#include <Storages/DeltaMerge/File/DMFileV3IncrementWriter.h>
#include <Storages/DeltaMerge/Index/IndexInfo.h>
#include <Storages/DeltaMerge/Index/VectorIndex.h>
#include <Storages/DeltaMerge/dtpb/dmfile.pb.h>
#include <Storages/PathPool.h>
#include <tipb/executor.pb.h>

#include <unordered_map>

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

namespace DB::DM
{

DMFileIndexWriter::LocalIndexBuildInfo DMFileIndexWriter::getLocalIndexBuildInfo(
    const LocalIndexInfosSnapshot & index_infos,
    const DMFiles & dm_files)
{
    assert(index_infos != nullptr);
    static constexpr double VECTOR_INDEX_SIZE_FACTOR = 1.2;

    LocalIndexBuildInfo build;
    build.indexes_to_build = std::make_shared<LocalIndexInfos>();
    build.file_ids.reserve(dm_files.size());
    for (const auto & dmfile : dm_files)
    {
        bool any_new_index_build = false;
        for (const auto & index : *index_infos)
        {
            auto col_id = index.column_id;
            const auto [state, data_bytes] = dmfile->getLocalIndexState(col_id, index.index_id);
            switch (state)
            {
            case DMFileMeta::LocalIndexState::NoNeed:
            case DMFileMeta::LocalIndexState::IndexBuilt:
                // The dmfile may be built before col_id is added, or has been built. Skip build indexes for it
                break;
            case DMFileMeta::LocalIndexState::IndexPending:
            {
                any_new_index_build = true;

                build.indexes_to_build->emplace_back(index);
                build.estimated_memory_bytes += data_bytes * VECTOR_INDEX_SIZE_FACTOR;
                break;
            }
            }
        }

        if (any_new_index_build)
        {
            build.file_ids.emplace_back(dmfile->fileId());
        }
    }

    build.file_ids.shrink_to_fit();
    return build;
}

void DMFileIndexWriter::buildIndexForFile(const DMFilePtr & dm_file_mutable, ProceedCheckFn should_proceed) const
{
    const auto column_defines = dm_file_mutable->getColumnDefines();
    const auto del_cd_iter = std::find_if(column_defines.cbegin(), column_defines.cend(), [](const ColumnDefine & cd) {
        return cd.id == TAG_COLUMN_ID;
    });
    RUNTIME_CHECK_MSG(
        del_cd_iter != column_defines.cend(),
        "Cannot find del_mark column, file={}",
        dm_file_mutable->path());

    // read_columns are: DEL_MARK, COL_A, COL_B, ...
    // index_builders are: COL_A -> {idx_M, idx_N}, COL_B -> {idx_O}, ...

    ColumnDefines read_columns{*del_cd_iter};
    read_columns.reserve(options.index_infos->size() + 1);

    std::unordered_map<IndexID, std::vector<VectorIndexBuilderPtr>> index_builders;

    std::unordered_map<ColId, std::vector<LocalIndexInfo>> col_indexes;
    for (const auto & index_info : *options.index_infos)
    {
        if (index_info.type != IndexType::Vector)
            continue;
        col_indexes[index_info.column_id].emplace_back(index_info);
    }

    for (const auto & [col_id, index_infos] : col_indexes)
    {
        const auto cd_iter = std::find_if(column_defines.cbegin(), column_defines.cend(), [&](const auto & cd) {
            return cd.id == col_id;
        });
        RUNTIME_CHECK_MSG(
            cd_iter != column_defines.cend(),
            "Cannot find column_id={} in file={}",
            col_id,
            dm_file_mutable->path());

        for (const auto & idx_info : index_infos)
        {
            // Index already built. We don't allow. The caller should filter away,
            RUNTIME_CHECK(
                !dm_file_mutable->isLocalIndexExist(idx_info.column_id, idx_info.index_id),
                idx_info.column_id,
                idx_info.index_id);
            index_builders[col_id].emplace_back(
                VectorIndexBuilder::create(idx_info.index_id, idx_info.index_definition));
        }
        read_columns.push_back(*cd_iter);
    }

    if (read_columns.size() == 1 || index_builders.empty())
    {
        // No index to build.
        return;
    }

    DMFileV3IncrementWriter::Options iw_options{
        .dm_file = dm_file_mutable,
        .file_provider = options.file_provider,
        .write_limiter = options.write_limiter,
        .path_pool = options.path_pool,
        .disagg_ctx = options.disagg_ctx,
    };
    auto iw = DMFileV3IncrementWriter::create(iw_options);

    // TODO: Maybe using DMFileReader directly is better because it doesn't need db_context.
    DMFileBlockInputStreamBuilder read_stream_builder(options.db_context);
    auto scan_context = std::make_shared<ScanContext>();

    // Note: We use range::newAll to build index for all data in dmfile, because the index is file-level.
    auto read_stream = read_stream_builder.build(
        dm_file_mutable,
        read_columns,
        {RowKeyRange::newAll(options.is_common_handle, options.rowkey_column_size)},
        scan_context);

    // Read all blocks and build index
    const size_t num_cols = read_columns.size();
    while (true)
    {
        if (!should_proceed())
            throw Exception(ErrorCodes::ABORTED, "Index build is interrupted");

        auto block = read_stream->read();
        if (!block)
            break;

        RUNTIME_CHECK(block.columns() == num_cols);
        RUNTIME_CHECK(block.getByPosition(0).column_id == TAG_COLUMN_ID);

        auto del_mark_col = block.safeGetByPosition(0).column;
        RUNTIME_CHECK(del_mark_col != nullptr);
        const auto * del_mark = static_cast<const ColumnVector<UInt8> *>(del_mark_col.get());
        RUNTIME_CHECK(del_mark != nullptr);

        for (size_t col_idx = 1; col_idx < num_cols; ++col_idx)
        {
            const auto & col_with_type_and_name = block.safeGetByPosition(col_idx);
            RUNTIME_CHECK(col_with_type_and_name.column_id == read_columns[col_idx].id);
            const auto & col = col_with_type_and_name.column;
            for (const auto & index_builder : index_builders[read_columns[col_idx].id])
            {
                index_builder->addBlock(*col, del_mark, should_proceed);
            }
        }
    }

    // Write down the index
    std::unordered_map<ColId, std::vector<dtpb::VectorIndexFileProps>> new_indexes_on_cols;
    for (size_t col_idx = 1; col_idx < num_cols; ++col_idx)
    {
        const auto & cd = read_columns[col_idx];
        // Save index and update column stats
        auto callback = [&](const IDataType::SubstreamPath & substream_path) -> void {
            if (IDataType::isNullMap(substream_path) || IDataType::isArraySizes(substream_path))
                return;

            std::vector<dtpb::VectorIndexFileProps> new_indexes;
            for (const auto & index_builder : index_builders[cd.id])
            {
                const IndexID index_id = index_builder->index_id;
                const auto index_file_name = index_id > 0
                    ? dm_file_mutable->vectorIndexFileName(index_id)
                    : colIndexFileName(DMFile::getFileNameBase(cd.id, substream_path));
                const auto index_path = iw->localPath() + "/" + index_file_name;
                index_builder->save(index_path);

                // Memorize what kind of vector index it is, so that we can correctly restore it when reading.
                dtpb::VectorIndexFileProps pb_idx;
                pb_idx.set_index_kind(tipb::VectorIndexKind_Name(index_builder->definition->kind));
                pb_idx.set_distance_metric(tipb::VectorDistanceMetric_Name(index_builder->definition->distance_metric));
                pb_idx.set_dimensions(index_builder->definition->dimension);
                pb_idx.set_index_id(index_id);
                pb_idx.set_index_bytes(Poco::File(index_path).getSize());
                new_indexes.emplace_back(std::move(pb_idx));

                iw->include(index_file_name);
            }
            // Inorder to avoid concurrency reading on ColumnStat, the new added indexes
            // will be insert into DMFile instance in `bumpMetaVersion`.
            new_indexes_on_cols.emplace(cd.id, std::move(new_indexes));
        };

        cd.type->enumerateStreams(callback);
    }

    dm_file_mutable->meta->bumpMetaVersion(DMFileMetaChangeset{new_indexes_on_cols});
    iw->finalize(); // Note: There may be S3 uploads here.
}

DMFiles DMFileIndexWriter::build(ProceedCheckFn should_proceed) const
{
    // Create a clone of existing DMFile instances by using DMFile::restore,
    // because later we will mutate some fields and persist these mutations.
    DMFiles cloned_dm_files{};

    auto delegate = options.path_pool->getStableDiskDelegator();
    for (const auto & dm_file : options.dm_files)
    {
        if (!options.disagg_ctx || !options.disagg_ctx->remote_data_store)
        {
            RUNTIME_CHECK(dm_file->parentPath() == delegate.getDTFilePath(dm_file->fileId()));
        }

        auto new_dmfile = DMFile::restore(
            options.file_provider,
            dm_file->fileId(),
            dm_file->pageId(),
            dm_file->parentPath(),
            DMFileMeta::ReadMode::all(),
            dm_file->metaVersion());

        cloned_dm_files.push_back(new_dmfile);
    }

    for (const auto & cloned_dmfile : cloned_dm_files)
    {
        buildIndexForFile(cloned_dmfile, should_proceed);
        // TODO: including the new index bytes in the file size.
        // auto res = dm_context.path_pool->getStableDiskDelegator().updateDTFileSize(
        //     new_dmfile->fileId(),
        //     new_dmfile->getBytesOnDisk());
        // RUNTIME_CHECK_MSG(res, "update dt file size failed, path={}", new_dmfile->path());
    }

    return cloned_dm_files;
}

} // namespace DB::DM
