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

#include <Storages/KVStore/MultiRaft/Spill/LargeTxnDefaultCf.h>

namespace DB
{

std::shared_ptr<LargeTxnDefaultCf::Inner> & LargeTxnDefaultCf::mustGet(
    LargeTxnDefaultCf & cf,
    const LargeTxnDefaultCf::Level1Key & key)
{
    if (!cf.txns.contains(key))
        cf.txns[key] = std::make_shared<LargeTxnDefaultCf::Inner>();
    return cf.txns[key];
}

RegionDataRes LargeTxnDefaultCf::insert(TiKVKey && key, TiKVValue && value, DupCheck mode)
{
    const auto & raw_key = RecordKVFormat::decodeTiKVKey(key);
    const auto & timestamp = RecordKVFormat::getTs(key);
    auto maybe_kv_pair = Trait::genKVPair(std::move(key), raw_key, std::move(value));
    if (!maybe_kv_pair)
        return 0;

    auto & kv_pair = maybe_kv_pair.value();
    return LargeTxnDefaultCf::mustGet(*this, timestamp)->doInsert(std::move(kv_pair), mode);
}

size_t LargeTxnDefaultCf::calcTiKVKeyValueSize(const Inner::Value & value)
{
    return calcTiKVKeyValueSize(Inner::getTiKVKey(value), Inner::getTiKVValue(value));
}
size_t LargeTxnDefaultCf::calcTiKVKeyValueSize(const TiKVKey & key, const TiKVValue & value)
{
    if constexpr (std::is_same<Trait, RegionLockCFDataTrait>::value)
    {
        std::ignore = key;
        std::ignore = value;
        return 0;
    }
    else
    {
        return key.dataSize() + value.dataSize();
    }
}

std::optional<LargeTxnDefaultCf::Inner::Map::const_iterator> LargeTxnDefaultCf::find(
    const Key & key,
    const Timestamp & ts) const
{
    auto t = txns.find(ts);
    if (t == txns.end())
    {
        return std::nullopt;
    }
    auto kv = t->second->getData().find(key);
    if (kv == t->second->getData().end())
    {
        return std::nullopt;
    }
    return kv;
}

size_t LargeTxnDefaultCf::getTiKVKeyValueSize(const Key & key, const Level1Key & ts) const
{
    auto maybe_kv = find(key, ts);
    if (maybe_kv.has_value())
    {
        return LargeTxnDefaultCf::Inner::calcTiKVKeyValueSize(maybe_kv.value()->second);
    }
    return 0;
}

size_t LargeTxnDefaultCf::remove(const Key & key, const Level1Key ts, bool quiet)
{
    auto & txn = txns.at(ts);
    return txn->remove(key, quiet);
}

void LargeTxnDefaultCf::erase(const Key & key, const Level1Key ts)
{
    auto & txn = txns.at(ts);
    txn->getDataMut().erase(key);
}

bool LargeTxnDefaultCf::cmp(const Map & a, const Map & b)
{
    if (a.size() != b.size())
        return false;
    for (auto it = a.cbegin(); it != a.cend(); it++)
    {
        if (auto it2 = b.find(it->first); it2 != b.end())
        {
            if (!Inner::cmp(it->second->getData(), it2->second->getData()))
                return false;
        }
        else
        {
            return false;
        }
    }
    return true;
}

bool LargeTxnDefaultCf::operator==(const LargeTxnDefaultCf & cf) const
{
    return cmp(txns, cf.txns);
}

size_t LargeTxnDefaultCf::getSize() const
{
    size_t size = 0;
    for (auto it = txns.begin(); it != txns.end(); it++)
    {
        size += it->second->getSize();
    }
    return size;
}

size_t LargeTxnDefaultCf::getTxnCount() const
{
    return txns.size();
}

size_t LargeTxnDefaultCf::splitInto(const RegionRange & range, LargeTxnDefaultCf & new_region_data)
{
    size_t size_changed = 0;
    for (auto it = txns.begin(); it != txns.end(); it++)
    {
        size_changed += it->second->splitInto(range, *LargeTxnDefaultCf::mustGet(new_region_data, it->first));
    }
    return size_changed;
}

size_t LargeTxnDefaultCf::mergeFrom(const LargeTxnDefaultCf & ori_region_data)
{
    size_t size_changed = 0;
    for (auto it = ori_region_data.txns.begin(); it != ori_region_data.txns.end(); it++)
    {
        size_changed += LargeTxnDefaultCf::mustGet(*this, it->first)->mergeFrom(*it->second);
    }
    return size_changed;
}

size_t LargeTxnDefaultCf::serializeMeta(WriteBuffer & buf) const
{
    size_t total_size = 0;

    size_t txn_count = getTxnCount();
    if likely (txn_count == 0)
        return 0;
    total_size += writeBinary2(txn_count, buf);
    return total_size;
}

size_t LargeTxnDefaultCf::serialize(WriteBuffer & buf) const
{
    size_t total_size = 0;

    for (const auto & e : txns)
    {
        total_size += writeBinary2(e.first, buf);
        total_size += e.second->serialize(buf);
    }

    return total_size;
}

size_t LargeTxnDefaultCf::deserializeMeta(ReadBuffer & buf)
{
    auto txn_count = readBinary2<size_t>(buf);
    return txn_count;
}

size_t LargeTxnDefaultCf::deserialize(ReadBuffer & buf, size_t txn_count, LargeTxnDefaultCf & new_region_data)
{
    size_t cf_data_size = 0;
    for (size_t i = 0; i < txn_count; i++)
    {
        auto timestamp = readBinary2<Timestamp>(buf);
        new_region_data.txns[timestamp] = std::make_shared<Inner>();
        cf_data_size += Inner::deserialize(buf, *new_region_data.txns[timestamp]);
    }
    return cf_data_size;
}

} // namespace DB