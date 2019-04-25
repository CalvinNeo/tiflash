#include <IO/MemoryReadWriteBuffer.h>
#include <Storages/Transaction/Region.h>
#include <Storages/Transaction/RegionPersister.h>

namespace DB
{

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
} // namespace ErrorCodes

void RegionPersister::drop(RegionID region_id)
{
    WriteBatch wb;
    wb.delPage(region_id);
    page_storage.write(wb);
}

void RegionPersister::persist(const RegionPtr & region, enginepb::CommandResponse * response)
{
    // Support only on thread persist.
    std::lock_guard<std::mutex> lock(mutex);

    size_t dirty_flag = region->dirtyFlag();
    doPersist(region, response);
    region->markPersisted();
    region->decDirtyFlag(dirty_flag);
}

void RegionPersister::doPersist(const RegionPtr & region, enginepb::CommandResponse * response)
{
    auto region_id = region->id();
    UInt64 applied_index = region->getIndex();

    auto cache = page_storage.getCache(region_id);
    if (cache.isValid() && cache.version > applied_index)
    {
        LOG_INFO(log, region->toString() << " have already persisted index: " << cache.version);
        return;
    }

    MemoryWriteBuffer buffer;
    size_t region_size = region->serialize(buffer, response);
    if (unlikely(region_size > std::numeric_limits<UInt32>::max()))
        throw Exception("Region is too big to persist", ErrorCodes::LOGICAL_ERROR);

    WriteBatch wb;
    auto read_buf = buffer.tryGetReadBuffer();
    wb.putPage(region_id, applied_index, read_buf, region_size);
    page_storage.write(wb);
}

void RegionPersister::restore(RegionMap & regions, RegionClientCreateFunc * func)
{
    auto acceptor = [&](const Page & page) {
        ReadBufferFromMemory buf(page.data.begin(), page.data.size());
        auto region = Region::deserialize(buf, func);
        if (page.page_id != region->id())
            throw Exception("region id and page id not match!", ErrorCodes::LOGICAL_ERROR);
        regions.emplace(page.page_id, region);
    };
    page_storage.traverse(acceptor);

    LOG_INFO(log, "restore " << regions.size() << " regions");
}

bool RegionPersister::gc() { return page_storage.gc(); }

} // namespace DB
