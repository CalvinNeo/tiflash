#pragma once

#include <Interpreters/SettingsCommon.h>
#include <Storages/Page/Page.h>
#include <Storages/Page/PageDefines.h>
#include <Storages/Page/PageStorage.h>
#include <Storages/Page/V2/PageFile.h>
#include <Storages/Page/V2/VersionSet/PageEntriesVersionSetWithDelta.h>
#include <Storages/Page/WriteBatch.h>

#include <condition_variable>
#include <functional>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

namespace DB
{
namespace PS::V2
{
/**
 * A storage system stored pages. Pages are serialized objects referenced by PageID. Store Page with the same PageID
 * will covered the old ones. The file used to persist the Pages called PageFile. The meta data of a Page, like the
 * latest PageFile the Page is stored , the offset in file, and checksum, are cached in memory. Users should call
 * #gc() constantly to clean up the sparse PageFiles and release disk space.
 *
 * This class is multi-threads safe. Support multi threads write, and multi threads read.
 */
class PageStorage : public DB::PageStorage
{
public:
    struct ListPageFilesOption
    {
        ListPageFilesOption()
            : remove_tmp_files(false)
            , ignore_legacy(false)
            , ignore_checkpoint(false)
            , remove_invalid_files(false)
        {}

        bool remove_tmp_files;
        bool ignore_legacy;
        bool ignore_checkpoint;
        bool remove_invalid_files;
    };

    using VersionedPageEntries = PageEntriesVersionSetWithDelta;
    using WriterPtr = std::unique_ptr<PageFile::Writer>;
    using ReaderPtr = std::shared_ptr<PageFile::Reader>;
    using OpenReadFiles = std::map<PageFileIdAndLevel, ReaderPtr>;

    using MetaMergingQueue
        = std::priority_queue<PageFile::MetaMergingReaderPtr, std::vector<PageFile::MetaMergingReaderPtr>, PageFile::MergingPtrComparator>;


    // Statistics for write
    struct StatisticsInfo
    {
        size_t puts = 0;
        size_t refs = 0;
        size_t deletes = 0;
        size_t upserts = 0;
        bool empty() const { return puts == 0 && refs == 0 && deletes == 0 && upserts == 0; }
        String toString() const;
        void mergeEdits(const PageEntriesEdit & edit);

        bool equals(const StatisticsInfo & rhs) const;
    };

public:
    PageStorage(String name,
                PSDiskDelegatorPtr delegator, //
                const Config & config_,
                const FileProviderPtr & file_provider_);
    ~PageStorage() = default;

    void restore() override;

    void drop() override;

    PageId getMaxId() override;

    PageId getNormalPageId(PageId page_id, SnapshotPtr snapshot) override;

    DB::PageStorage::SnapshotPtr getSnapshot() override;

    using ConcreteSnapshotRawPtr = VersionedPageEntries::Snapshot *;
    using ConcreteSnapshotPtr = VersionedPageEntries::SnapshotPtr;
    ConcreteSnapshotPtr getConcreteSnapshot();

    std::tuple<size_t, double, unsigned> getSnapshotsStat() const override;

    void write(DB::WriteBatch && wb, const WriteLimiterPtr & write_limiter) override;

    DB::PageEntry getEntry(PageId page_id, SnapshotPtr snapshot) override;

    DB::Page read(PageId page_id, const ReadLimiterPtr & read_limiter, SnapshotPtr snapshot) override;

    PageMap read(const std::vector<PageId> & page_ids, const ReadLimiterPtr & read_limiter, SnapshotPtr snapshot) override;

    void read(const std::vector<PageId> & page_ids, const PageHandler & handler, const ReadLimiterPtr & read_limiter, SnapshotPtr snapshot) override;

    PageMap read(const std::vector<PageReadFields> & page_fields, const ReadLimiterPtr & read_limiter, SnapshotPtr snapshot) override;

    void traverse(const std::function<void(const DB::Page & page)> & acceptor, SnapshotPtr snapshot) override;

    bool gc(bool not_skip, const WriteLimiterPtr & write_limiter, const ReadLimiterPtr & read_limiter) override;

    void registerExternalPagesCallbacks(const ExternalPageCallbacks & callbacks) override;

    FileProviderPtr getFileProvider() const { return file_provider; }

    static PageFileSet listAllPageFiles(
        const FileProviderPtr & file_provider,
        PSDiskDelegatorPtr & delegator,
        Poco::Logger * page_file_log,
        const ListPageFilesOption & option = ListPageFilesOption());

    static PageFormat::Version getMaxDataVersion(const FileProviderPtr & file_provider, PSDiskDelegatorPtr & delegator)
    {
        Poco::Logger * log = &Poco::Logger::get("PageStorage::getMaxDataVersion");
        ListPageFilesOption option;
        option.ignore_checkpoint = true;
        option.ignore_legacy = true;
        option.remove_tmp_files = false;
        auto page_files = listAllPageFiles(file_provider, delegator, log, option);
        if (page_files.empty())
            return STORAGE_FORMAT_CURRENT.page;

        bool all_empty = true;
        PageFormat::Version max_binary_version = PageFormat::V1;
        PageFormat::Version temp_version = STORAGE_FORMAT_CURRENT.page;
        for (auto iter = page_files.rbegin(); iter != page_files.rend(); ++iter)
        {
            // Skip those files without valid meta
            if (iter->getMetaFileSize() == 0)
                continue;

            // Simply check the last non-empty PageFile is good enough
            all_empty = false;
            auto reader = PageFile::MetaMergingReader::createFrom(const_cast<PageFile &>(*iter));
            while (reader->hasNext())
            {
                // Continue to read the binary version of next WriteBatch.
                reader->moveNext(&temp_version);
                max_binary_version = std::max(max_binary_version, temp_version);
            }
            LOG_DEBUG(log, "getMaxDataVersion done from " + reader->toString() << " [max version=" << max_binary_version << "]");
            break;
        }
        max_binary_version = (all_empty ? STORAGE_FORMAT_CURRENT.page : max_binary_version);
        return max_binary_version;
    }

    struct PersistState
    {
        // use to protect reading WriteBatches from writable PageFile's meta in GC
        size_t meta_offset = 0;
        // use to protect that legacy compactor won't exceed the sequence of minimum persisted
        WriteBatch::SequenceID sequence = 0;
    };

    struct WritingFilesSnapshot
    {
        using const_iterator = std::map<PageFileIdAndLevel, PersistState>::const_iterator;

        PageFileIdAndLevel minFileIDLevel() const;
        WriteBatch::SequenceID minPersistedSequence() const;

        const_iterator find(const PageFileIdAndLevel & id) const { return states.find(id); }
        const_iterator end() const { return states.end(); }
        bool contains(const PageFileIdAndLevel & id) const { return states.count(id) > 0; }

        std::map<PageFileIdAndLevel, PersistState> states;
    };

#ifndef NDEBUG
    // Just for tests, refactor them out later
    void write(DB::WriteBatch && wb) { return write(std::move(wb), nullptr); }
    DB::PageEntry getEntry(PageId page_id) { return getEntry(page_id, nullptr); }
    DB::Page read(PageId page_id) { return read(page_id, nullptr, nullptr); }
    PageMap read(const std::vector<PageId> & page_ids) { return read(page_ids, nullptr, nullptr); }
    void read(const std::vector<PageId> & page_ids, const PageHandler & handler) { return read(page_ids, handler, nullptr, nullptr); }
    PageMap read(const std::vector<PageReadFields> & page_fields) { return read(page_fields, nullptr, nullptr); }
    void traverse(const std::function<void(const DB::Page & page)> & acceptor) { return traverse(acceptor, nullptr); }
    bool gc() { return gc(false, nullptr, nullptr); }
#endif

#ifndef DBMS_PUBLIC_GTEST
private:
#endif
    WriterPtr checkAndRenewWriter(PageFile & page_file,
                                  const String & parent_path_hint,
                                  WriterPtr && old_writer = nullptr,
                                  const String & logging_msg = "");
    ReaderPtr getReader(const PageFileIdAndLevel & file_id_level);

    static constexpr const char * ARCHIVE_SUBDIR = "archive";

    void archivePageFiles(const PageFileSet & page_files_to_archive, bool remove_size);

    std::tuple<size_t, size_t> //
    gcRemoveObsoleteData(PageFileSet & page_files,
                         const PageFileIdAndLevel & writing_file_id_level,
                         const std::set<PageFileIdAndLevel> & live_files);

    void getWritingSnapshot(std::lock_guard<std::mutex> &, WritingFilesSnapshot & writing_snapshot) const;

    friend class LegacyCompactor;

    template <typename SnapshotPtr>
    friend class DataCompactor;

#ifndef DBMS_PUBLIC_GTEST
private:
#endif
    struct WritingPageFile
    {
        PageFile file;
        PersistState persisted{};
    };
    std::mutex write_mutex; // A mutex protect `idle_writers`,`write_files` and `statistics`.

    // TODO: Wrap `write_mutex_cv`, `write_files`, `idle_writers` to be a standalone class
    std::condition_variable write_mutex_cv;
    std::vector<WritingPageFile> write_files;
    std::deque<WriterPtr> idle_writers;
    StatisticsInfo statistics;

    // A sequence number to keep ordering between multi-writers.
    std::atomic<WriteBatch::SequenceID> write_batch_seq = 0;

    OpenReadFiles open_read_files;
    std::mutex open_read_files_mutex; // A mutex only used to protect open_read_files.

    Poco::Logger * page_file_log;
    Poco::Logger * log;

    VersionedPageEntries versioned_page_entries;

    std::atomic<bool> gc_is_running = false;

    ExternalPageCallbacks::ExternalPagesScanner external_pages_scanner = nullptr;
    ExternalPageCallbacks::ExternalPagesRemover external_pages_remover = nullptr;

    StatisticsInfo last_gc_statistics;

private:
    WriterPtr checkAndRenewWriter(
        WritingPageFile & writing_file,
        const String & parent_path_hint,
        WriterPtr && old_writer = nullptr,
        const String & logging_msg = "");
};

} // namespace PS::V2
} // namespace DB
