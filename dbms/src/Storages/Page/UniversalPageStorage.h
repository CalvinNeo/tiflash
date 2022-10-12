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

#pragma once

#include <Storages/Page/ExternalPageCallbacks.h>
#include <Storages/Page/FileUsage.h>
#include <Storages/Page/PageDefines.h>
#include <Storages/Page/Snapshot.h>
#include <Storages/Page/UniversalPage.h>
#include <Storages/Page/UniversalWriteBatch.h>
#include <Storages/Page/V3/BlobStore.h>
#include <Storages/Page/V3/PageDirectory.h>
#include <common/defines.h>

namespace DB
{
class FileProvider;
using FileProviderPtr = std::shared_ptr<FileProvider>;
class PathCapacityMetrics;
using PathCapacityMetricsPtr = std::shared_ptr<PathCapacityMetrics>;
class PSDiskDelegator;
using PSDiskDelegatorPtr = std::shared_ptr<PSDiskDelegator>;
class Context;
class WriteLimiter;
using WriteLimiterPtr = std::shared_ptr<WriteLimiter>;
class ReadLimiter;
using ReadLimiterPtr = std::shared_ptr<ReadLimiter>;

class UniversalPageStorage;
using UniversalPageStoragePtr = std::shared_ptr<UniversalPageStorage>;

class KVStoreReader;

class UniversalPageStorage final
{
public:
    using SnapshotPtr = PageStorageSnapshotPtr;
    struct Config
    {
    };

public:
    static UniversalPageStoragePtr
    create(
        String name,
        PSDiskDelegatorPtr delegator,
        const UniversalPageStorage::Config & config,
        const FileProviderPtr & file_provider);

    UniversalPageStorage(
        String name,
        PSDiskDelegatorPtr delegator_,
        const Config & config_,
        const FileProviderPtr & file_provider_)
        : storage_name(std::move(name))
        , delegator(std::move(delegator_))
        , config(config_)
        , file_provider(file_provider_)
    {
    }

    ~UniversalPageStorage() = default;

    void restore();

    SnapshotPtr getSnapshot(const String & tracing_id) const
    {
        return page_directory->createSnapshot(tracing_id);
    }

    // Get some statistics of all living snapshots and the oldest living snapshot.
    SnapshotsStatistics getSnapshotsStat() const
    {
        return page_directory->getSnapshotsStat();
    }

    FileUsageStatistics getFileUsageStatistics() const
    {
        return blob_store->getFileUsageStatistics();
    }

    void write(UniversalWriteBatch && write_batch, const WriteLimiterPtr & write_limiter = nullptr)
    {
        UNUSED(write_batch, write_limiter);
    }

    UniversalPageMap read(const UniversalPageId & page_ids, const ReadLimiterPtr & read_limiter = nullptr, SnapshotPtr snapshot = {})
    {
        UNUSED(page_ids, read_limiter, snapshot);
        throw Exception("", ErrorCodes::NOT_IMPLEMENTED);
    }

    using FieldIndices = std::vector<size_t>;
    using PageReadFields = std::pair<UniversalPageId, FieldIndices>;

    UniversalPageMap read(const std::vector<PageReadFields> & page_fields, const ReadLimiterPtr & read_limiter = nullptr, SnapshotPtr snapshot = {})
    {
        UNUSED(page_fields, read_limiter, snapshot);
        throw Exception("", ErrorCodes::NOT_IMPLEMENTED);
    }

    UniversalPage read(const PageReadFields & page_field, const ReadLimiterPtr & read_limiter = nullptr, SnapshotPtr snapshot = {})
    {
        UNUSED(page_field, read_limiter, snapshot);
        throw Exception("", ErrorCodes::NOT_IMPLEMENTED);
    }

    // We may skip the GC to reduce useless reading by default.
    bool gc(bool not_skip = false, const WriteLimiterPtr & write_limiter = nullptr, const ReadLimiterPtr & read_limiter = nullptr)
    {
        UNUSED(not_skip, write_limiter, read_limiter);
        return false;
    }

    // Register and unregister external pages GC callbacks
    // Note that user must ensure that it is safe to call `scanner` and `remover` even after unregister.
    void registerExternalPagesCallbacks(const ExternalPageCallbacks & callbacks) { UNUSED(callbacks); }
    void unregisterExternalPagesCallbacks(NamespaceId /*ns_id*/) {}

    String storage_name; // Identify between different Storage
    PSDiskDelegatorPtr delegator; // Get paths for storing data
    Config config;
    FileProviderPtr file_provider;

    PS::V3::universal::PageDirectoryPtr page_directory;
    PS::V3::universal::BlobStorePtr blob_store;
};

class KVStoreReader
{
public:
    KVStoreReader(UniversalPageStorage & storage)
        : uni_storage(storage)
    {}

    bool isAppliedIndexExceed(PageId page_id, UInt64 applied_index)
    {
        // assume use this format
        UniversalPageId uni_page_id = fmt::format("k_{}", page_id);
        auto snap = uni_storage.getSnapshot("");
        const auto & [id, entry] = uni_storage.page_directory->getByIDOrNull(uni_page_id, snap);
        return (entry.isValid() && entry.tag > applied_index);
    }

    void traverse(const std::function<void(const DB::UniversalPage & page)> & /*acceptor*/)
    {
        // Only traverse pages with id prefix
        throw Exception("", ErrorCodes::NOT_IMPLEMENTED);
    }

private:
    UniversalPageStorage & uni_storage;
};

} // namespace DB
