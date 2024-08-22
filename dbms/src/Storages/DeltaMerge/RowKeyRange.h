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
#include <Columns/ColumnString.h>
#include <Common/SharedMutexProtected.h>
#include <Core/Types.h>
#include <Functions/FunctionHelpers.h>
#include <IO/WriteHelpers.h>
#include <Storages/DeltaMerge/DeltaMergeDefines.h>
#include <Storages/DeltaMerge/DeltaMergeHelpers.h>
#include <Storages/DeltaMerge/dtpb/column_file.pb.h>
#include <Storages/KVStore/Decode/DecodedTiKVKeyValue.h>
#include <Storages/KVStore/MultiRaft/RegionRangeKeys.h>
#include <Storages/KVStore/TiKVHelpers/TiKVRecordFormat.h>
#include <Storages/KVStore/Types.h>
#include <TiDB/Decode/DatumCodec.h>

namespace DB::DM
{
using HandleValuePtr = std::shared_ptr<String>;

struct RowKeyRange;
using RowKeyRanges = std::vector<RowKeyRange>;

namespace CommonHandle
{
inline std::strong_ordering compare(const char * a, size_t a_size, const char * b, size_t b_size)
{
    int res = memcmp(a, b, std::min(a_size, b_size));
    if (res < 0)
    {
        return std::strong_ordering::less;
    }
    else if (res > 0)
    {
        return std::strong_ordering::greater;
    }
    else
    {
        return a_size <=> b_size;
    }
}
} // namespace CommonHandle

struct RowKeyValue;

/// A reference to RowKeyValue, normally the real data value is stored in a column.
struct RowKeyValueRef
{
    /// when is_common_handle = true, it means the table has clustered index, the rowkey value is a string
    /// when is_common_handle = false, it means the table has int/uint handle, the rowkey value is a int/uint
    bool is_common_handle;
    /// data in RowKeyValue can be nullptr if is_common_handle is false
    /// generally speaking:
    /// if the RowKeyValueRef comes from handle column, data is nullptr,
    /// if the RowKeyValueRef comes from meta data, the data is not nullptr
    const char * data;
    size_t size;
    Int64 int_value;

    // Format as a hex string for debugging. The value will be converted to '?' if redact-log is on
    String toDebugString() const;

    RowKeyValue toRowKeyValue() const;
};

/// Stores the raw bytes of the RowKey. Normally the RowKey will be stored in a column.
struct RowKeyValue
{
    RowKeyValue() = default;
    RowKeyValue(bool is_common_handle_, HandleValuePtr value_, Int64 int_value_)
        : is_common_handle(is_common_handle_)
        , value(value_)
        , int_value(int_value_)
    {}

    RowKeyValue(bool is_common_handle_, HandleValuePtr value_)
        : is_common_handle(is_common_handle_)
        , value(value_)
    {
        if (is_common_handle)
            int_value = 0;
        else
        {
            size_t cursor = 0;
            int_value = DB::DecodeInt64(cursor, *value);
            if (unlikely(value->size() != sizeof(Int64)))
            {
                // For int type handle, the standard key enconding format should be t{table_id}_r{handle_value}.
                // But TiKV may generate region range keys which are not strictly following the standard format.
                // More concretely, the key may be t{table_id}_r{handle_value} + some other bytes.
                // We need to adapt the key to the standard format.
                // For example, the key may be t100_r1000 + 0x00, we need to adapt it to t100_r1001.
                // This is ok, because
                //  1) if the key is the start range, then [t100_r1000 + 0x00, xxx) has the same semantics with [t100_r1001, xxx)
                //  2) if the key is the end range, then [xxx, t100_r1000 + 0x00) also has the same semantics with [xxx, t100_r1001)
                //
                // Note if the `int_value` is Int64::max_value,
                // it is a value generated by tiflash itself to means +inf()(which is RowKeyValue::INT_HANDLE_MAX_KEY).
                // So we can just ignore it.
                if (value->size() != sizeof(UInt64) + 1 || value->back() != 0x00)
                {
                    LOG_WARNING(
                        Logger::get(),
                        "Meet rowkey {} with unexpected encoding format",
                        Redact::keyToDebugString(value->data(), value->size()));
                }
                else
                {
                    if (int_value < std::numeric_limits<Int64>::max())
                    {
                        LOG_WARNING(
                            Logger::get(),
                            "Meet rowkey {} which has an extra zero suffix",
                            Redact::keyToDebugString(value->data(), value->size()));
                        int_value = int_value + 1;
                        WriteBufferFromOwnString ss;
                        DB::EncodeInt64(int_value, ss);
                        value = std::make_shared<String>(ss.releaseStr());
                    }
                    else
                    {
                        // ignore RowKeyValue::INT_HANDLE_MAX_KEY
                    }
                }
            }
        }
    }

    explicit RowKeyValue(const RowKeyValueRef & rowkey_value)
    {
        is_common_handle = rowkey_value.is_common_handle;
        if (is_common_handle)
            value = std::make_shared<String>(rowkey_value.data, rowkey_value.size);
        else
        {
            WriteBufferFromOwnString ss;
            DB::EncodeInt64(rowkey_value.int_value, ss);
            value = std::make_shared<String>(ss.releaseStr());
        }
        int_value = rowkey_value.int_value;
    }

    static RowKeyValue fromHandle(Handle value)
    {
        WriteBufferFromOwnString ss;
        DB::EncodeInt64(value, ss);
        return RowKeyValue(false, std::make_shared<String>(ss.releaseStr()), value);
    }

    // Format as a string
    String toString() const;

    // Format as a hex string for debugging. The value will be converted to '?' if redact-log is on
    String toDebugString() const;

    inline RowKeyValueRef toRowKeyValueRef() const
    {
        return RowKeyValueRef{is_common_handle, value->data(), value->size(), int_value};
    }

    DecodedTiKVKeyPtr toRegionKey(TableID table_id) const
    {
        // FIXME: move this to TiKVRecordFormat.h
        WriteBufferFromOwnString ss;
        ss.write('t');
        EncodeInt64(table_id, ss);
        ss.write('_');
        ss.write('r');
        String prefix = ss.releaseStr();
        return std::make_shared<DecodedTiKVKey>(prefix + *value);
    }

    bool operator==(const RowKeyValue & v) const
    {
        return is_common_handle == v.is_common_handle && (*value) == (*v.value) && int_value == v.int_value;
    }
    /**
     * Returns the key so that the range [this, this.toPrefixNext()) contains
     * all keys with the prefix `this`.
     */
    RowKeyValue toPrefixNext() const
    {
        std::vector<UInt8> keys(value->begin(), value->end());
        int index = keys.size() - 1;
        for (; index >= 0; index--)
        {
            if (keys[index] == std::numeric_limits<UInt8>::max())
                keys[index] = 0;
            else
            {
                keys[index] = keys[index] + 1;
                break;
            }
        }
        if (index == -1)
        {
            keys.clear();
            keys.insert(keys.end(), value->begin(), value->end());
            keys.push_back(0);
        }
        HandleValuePtr prefix_value = std::make_shared<String>(keys.begin(), keys.end());
        Int64 prefix_int_value = int_value;
        if (!is_common_handle && prefix_int_value != std::numeric_limits<Int64>::max())
        {
            prefix_int_value++;
        }
        return RowKeyValue(is_common_handle, prefix_value, prefix_int_value);
    }

    /**
     * Returns the smallest row key which is larger than the current row key.
     */
    RowKeyValue toNext() const
    {
        // We want to always ensure that the IntHandle.stringValue == IntHandle.intValue.
        if (!is_common_handle)
            return toPrefixNext();

        HandleValuePtr next_value = std::make_shared<String>(value->begin(), value->end());
        next_value->push_back(0x0);

        // For common handle, int_value will not be used in compare. Let's keep it unchanged.
        return RowKeyValue(/* is_common_handle */ true, next_value, int_value);
    }

    void serialize(WriteBuffer & buf) const
    {
        writeBoolText(is_common_handle, buf);
        writeStringBinary(*value, buf);
    }

    static RowKeyValue deserialize(ReadBuffer & buf)
    {
        bool is_common_handle;
        String value;
        readBoolText(is_common_handle, buf);
        readStringBinary(value, buf);
        HandleValuePtr start_ptr = std::make_shared<String>(value);
        return RowKeyValue(is_common_handle, start_ptr);
    }

    bool is_common_handle = false;
    /// In case of non common handle, the value field is redundant in most cases, except that int_value == Int64::max_value,
    /// because RowKeyValue is an end point of RowKeyRange, assuming that RowKeyRange = [start_value, end_value), since the
    /// end_value of RowKeyRange is always exclusive, if we want to construct a RowKeyRange that include Int64::max_value,
    /// just set end_value.int_value to Int64::max_value is not enough, we still need to set end_value.value a carefully
    /// designed value. You can refer to INT_HANDLE_MAX_KEY for more details
    HandleValuePtr value;
    Int64 int_value = 0;

    static const RowKeyValue INT_HANDLE_MIN_KEY;
    static const RowKeyValue INT_HANDLE_MAX_KEY;
    static const RowKeyValue COMMON_HANDLE_MIN_KEY;
    static const RowKeyValue COMMON_HANDLE_MAX_KEY;
    static const RowKeyValue EMPTY_STRING_KEY;
};

using RowKeyValues = std::vector<RowKeyValue>;

/// An optimized implementation that will try to compare IntHandle via comparing Int values directly.
/// For common handles, per-byte comparison will be still used.
inline std::strong_ordering operator<=>(const RowKeyValueRef & a, const RowKeyValueRef & b)
{
    if (unlikely(a.is_common_handle != b.is_common_handle))
        throw Exception(
            "Should not reach here, common handle rowkey value compare with non common handle rowkey value");
    if (a.is_common_handle)
    {
        return CommonHandle::compare(a.data, a.size, b.data, b.size);
    }
    else
    {
        /// in case of non common handle, we can compare the int value directly in most cases
        if (a.int_value != b.int_value
            || (a.int_value != RowKeyValue::INT_HANDLE_MAX_KEY.int_value || (a.data == nullptr && b.data == nullptr)))
            return a.int_value <=> b.int_value;

        /// if a.int_value == b.int_value == Int64::max_value, we need to further check the data field because even if
        /// the RowKeyValueRef is bigger that Int64::max_value, the int_value field is Int64::max_value at most.
        bool a_inf = false;
        bool b_inf = false;
        if (a.data != nullptr)
            a_inf = CommonHandle::compare(
                        a.data,
                        a.size,
                        RowKeyValue::INT_HANDLE_MAX_KEY.value->data(),
                        RowKeyValue::INT_HANDLE_MAX_KEY.value->size())
                == std::strong_ordering::equal;
        if (b.data != nullptr)
            b_inf = CommonHandle::compare(
                        b.data,
                        b.size,
                        RowKeyValue::INT_HANDLE_MAX_KEY.value->data(),
                        RowKeyValue::INT_HANDLE_MAX_KEY.value->size())
                == std::strong_ordering::equal;

        return static_cast<int>(a_inf) <=> static_cast<int>(b_inf);
    }
}

inline bool operator==(const RowKeyValueRef & a, const RowKeyValueRef & b)
{
    return (a <=> b) == std::strong_ordering::equal;
}

inline bool operator!=(const RowKeyValueRef & a, const RowKeyValueRef & b)
{
    return (a <=> b) != std::strong_ordering::equal;
}

// For compare `RowKeyValue`, do not use operator<=> of `RowKeyValueRef`.
// Because `is_common_handle` of `a` and `b` may not match.
inline auto operator<=>(const RowKeyValue & a, const RowKeyValue & b)
{
    // Seems std::string::operator<=> is not support in clang-15.
    return CommonHandle::compare(a.value->data(), a.value->size(), b.value->data(), b.value->size());
}

struct RowKeyColumnContainer
{
    const ColumnPtr & column;
    bool is_common_handle;
    bool is_constant_column;

    /// The following members are simply references to values in 'column', for faster access.
    const ColumnString::Chars_t * string_data = nullptr;
    const ColumnString::Offsets * string_offsets = nullptr;
    const PaddedPODArray<Int64> * int_data = nullptr;

    RowKeyColumnContainer(const ColumnPtr & column_, bool is_common_handle_)
        : column(column_)
        , is_common_handle(is_common_handle_)
        , is_constant_column(column->isColumnConst())
    {
        const IColumn * non_const_column_ptr = column.get();
        if (unlikely(is_constant_column))
        {
            non_const_column_ptr = checkAndGetColumn<ColumnConst>(column.get())->getDataColumnPtr().get();
        }


        if (is_common_handle)
        {
            const auto & column_string = *checkAndGetColumn<ColumnString>(non_const_column_ptr);
            string_data = &column_string.getChars();
            string_offsets = &column_string.getOffsets();
        }
        else
        {
            int_data = &toColumnVectorData<Int64>(*non_const_column_ptr);
        }
    }

    RowKeyValueRef getRowKeyValue(size_t index) const
    {
        // todo check index out of bound error
        if (unlikely(is_constant_column))
            index = 0;
        if (is_common_handle)
        {
            size_t prev_offset = index == 0 ? 0 : (*string_offsets)[index - 1];
            return RowKeyValueRef{
                is_common_handle,
                reinterpret_cast<const char *>(&(*string_data)[prev_offset]),
                (*string_offsets)[index] - prev_offset - 1,
                0};
        }
        else
        {
            Int64 int_value = (*int_data)[index];
            return RowKeyValueRef{is_common_handle, nullptr, 0, int_value};
        }
    }
};

/// A range denoted as [StartRowKey, EndRowKey).
struct RowKeyRange
{
    // TODO: use template to refine is_common_handle
    bool is_common_handle;

    // start and end in RowKeyRange are always meaningful.
    RowKeyValue start;
    RowKeyValue end;
    size_t rowkey_column_size;

    struct TableRangeMinMax
    {
        HandleValuePtr min;
        HandleValuePtr max;

        TableRangeMinMax(KeyspaceID keyspace_id, TableID table_id, bool is_common_handle)
        {
            WriteBufferFromOwnString ss;
            auto ks_pfx = DecodedTiKVKey::makeKeyspacePrefix(keyspace_id);
            ss.write(ks_pfx.data(), ks_pfx.size());
            ss.write('t');
            EncodeInt64(table_id, ss);
            ss.write('_');
            ss.write('r');
            String prefix = ss.releaseStr();
            if (is_common_handle)
            {
                min = std::make_shared<String>(prefix + *RowKeyValue::COMMON_HANDLE_MIN_KEY.value);
                max = std::make_shared<String>(prefix + *RowKeyValue::COMMON_HANDLE_MAX_KEY.value);
            }
            else
            {
                min = std::make_shared<String>(prefix + *RowKeyValue::INT_HANDLE_MIN_KEY.value);
                max = std::make_shared<String>(prefix + *RowKeyValue::INT_HANDLE_MAX_KEY.value);
            }
        }
    };

    /// maybe use a LRU cache in case there are massive tables
    static SharedMutexProtected<
        std::unordered_map<KeyspaceTableID, RowKeyRange::TableRangeMinMax, boost::hash<KeyspaceTableID>>>
        table_min_max_data;
    static const TableRangeMinMax & getTableMinMaxData(KeyspaceID keyspace_id, TableID table_id, bool is_common_handle);

    RowKeyRange(
        const RowKeyValue & start_,
        const RowKeyValue & end_,
        bool is_common_handle_,
        size_t rowkey_column_size_)
        : is_common_handle(is_common_handle_)
        , start(start_)
        , end(end_)
        , rowkey_column_size(rowkey_column_size_)
    {}

    RowKeyRange()
        : is_common_handle(true)
        , start(RowKeyValue::EMPTY_STRING_KEY)
        , end(RowKeyValue::EMPTY_STRING_KEY)
        , rowkey_column_size(0)
    {}

    void swap(RowKeyRange & other)
    {
        std::swap(is_common_handle, other.is_common_handle);
        std::swap(start, other.start);
        std::swap(end, other.end);
        std::swap(rowkey_column_size, other.rowkey_column_size);
    }

    static RowKeyRange startFrom(const RowKeyValueRef & start_value, bool is_common_handle, size_t rowkey_column_size)
    {
        if (is_common_handle)
        {
            return RowKeyRange(
                start_value.toRowKeyValue(),
                RowKeyValue::COMMON_HANDLE_MAX_KEY,
                is_common_handle,
                rowkey_column_size);
        }
        else
        {
            return RowKeyRange(
                start_value.toRowKeyValue(),
                RowKeyValue::INT_HANDLE_MAX_KEY,
                is_common_handle,
                rowkey_column_size);
        }
    }

    static RowKeyRange endWith(const RowKeyValueRef & end_value, bool is_common_handle, size_t rowkey_column_size)
    {
        if (is_common_handle)
        {
            return RowKeyRange(
                RowKeyValue::COMMON_HANDLE_MIN_KEY,
                end_value.toRowKeyValue(),
                is_common_handle,
                rowkey_column_size);
        }
        else
        {
            return RowKeyRange(
                RowKeyValue::INT_HANDLE_MIN_KEY,
                end_value.toRowKeyValue(),
                is_common_handle,
                rowkey_column_size);
        }
    }

    /// Create a RowKeyRange that covers all key space.
    static RowKeyRange newAll(bool is_common_handle, size_t rowkey_column_size)
    {
        if (is_common_handle)
        {
            return RowKeyRange(
                RowKeyValue::COMMON_HANDLE_MIN_KEY,
                RowKeyValue::COMMON_HANDLE_MAX_KEY,
                is_common_handle,
                rowkey_column_size);
        }
        else
        {
            return RowKeyRange(
                RowKeyValue::INT_HANDLE_MIN_KEY,
                RowKeyValue::INT_HANDLE_MAX_KEY,
                is_common_handle,
                rowkey_column_size);
        }
    }

    /// Create a RowKeyRange that covers no data at all.
    static RowKeyRange newNone(bool is_common_handle, size_t rowkey_column_size)
    {
        if (is_common_handle)
        {
            return RowKeyRange(
                RowKeyValue::COMMON_HANDLE_MAX_KEY,
                RowKeyValue::COMMON_HANDLE_MIN_KEY,
                is_common_handle,
                rowkey_column_size);
        }
        else
        {
            return RowKeyRange(
                RowKeyValue::INT_HANDLE_MAX_KEY,
                RowKeyValue::INT_HANDLE_MIN_KEY,
                is_common_handle,
                rowkey_column_size);
        }
    }

    void serialize(WriteBuffer & buf) const
    {
        writeBoolText(is_common_handle, buf);
        writeIntBinary(rowkey_column_size, buf);
        writeStringBinary(*start.value, buf);
        writeStringBinary(*end.value, buf);
    }

    dtpb::RowKeyRange serialize() const
    {
        dtpb::RowKeyRange range;
        range.set_is_common_handle(is_common_handle);
        range.set_rowkey_column_size(rowkey_column_size);
        range.set_start(start.value->data(), start.value->size());
        range.set_end(end.value->data(), end.value->size());
        return range;
    }

    static RowKeyRange deserialize(ReadBuffer & buf)
    {
        bool is_common_handle;
        size_t rowkey_column_size;
        String start, end;
        readBoolText(is_common_handle, buf);
        readIntBinary(rowkey_column_size, buf);
        readStringBinary(start, buf);
        readStringBinary(end, buf);
        HandleValuePtr start_ptr = std::make_shared<String>(start);
        HandleValuePtr end_ptr = std::make_shared<String>(end);
        if unlikely (isLegacyCommonMin(rowkey_column_size, start_ptr))
        {
            start_ptr = RowKeyValue::COMMON_HANDLE_MIN_KEY.value;
        }
        if unlikely (isLegacyCommonMax(rowkey_column_size, end_ptr))
        {
            end_ptr = RowKeyValue::COMMON_HANDLE_MAX_KEY.value;
        }
        return RowKeyRange(
            RowKeyValue(is_common_handle, start_ptr),
            RowKeyValue(is_common_handle, end_ptr),
            is_common_handle,
            rowkey_column_size);
    }

    static RowKeyRange deserialize(const dtpb::RowKeyRange & range)
    {
        bool is_common_handle = range.is_common_handle();
        size_t rowkey_column_size = range.rowkey_column_size();
        HandleValuePtr start_ptr = std::make_shared<String>(range.start());
        HandleValuePtr end_ptr = std::make_shared<String>(range.end());
        if unlikely (isLegacyCommonMin(rowkey_column_size, start_ptr))
        {
            start_ptr = RowKeyValue::COMMON_HANDLE_MIN_KEY.value;
        }
        if unlikely (isLegacyCommonMax(rowkey_column_size, end_ptr))
        {
            end_ptr = RowKeyValue::COMMON_HANDLE_MAX_KEY.value;
        }
        return RowKeyRange(
            RowKeyValue(is_common_handle, start_ptr),
            RowKeyValue(is_common_handle, end_ptr),
            is_common_handle,
            rowkey_column_size);
    }

    static bool isLegacyCommonMin(size_t column_size, HandleValuePtr value)
    {
        if (column_size > 0)
        {
            if (value->size() != column_size)
                return false;
            for (size_t i = 0; i < column_size && i < value->size(); i++)
            {
                if (static_cast<unsigned char>((*value)[i]) != TiDB::CodecFlagBytes
                    && static_cast<unsigned char>((*value)[i]) != TiDB::CodecFlag::CodecFlagNil)
                    return false;
            }
            return true;
        }
        return false;
    }
    static bool isLegacyCommonMax(size_t column_size, HandleValuePtr value)
    {
        if (column_size > 0)
        {
            if (value->size() != column_size)
                return false;
            for (size_t i = 0; i < column_size && i < value->size(); i++)
            {
                if (static_cast<unsigned char>((*value)[i]) != TiDB::CodecFlagMax)
                    return false;
            }
            return true;
        }
        return false;
    }

    inline bool all() const { return isStartInfinite() && isEndInfinite(); }

    inline bool isEndInfinite() const
    {
        if (is_common_handle)
        {
            return (end.value->size() == 1 && static_cast<unsigned char>((*end.value)[0]) == TiDB::CodecFlagMax);
        }
        else
        {
            return end.int_value == RowKeyValue::INT_HANDLE_MAX_KEY.int_value
                && *end.value >= *RowKeyValue::INT_HANDLE_MAX_KEY.value;
        }
    }

    inline bool isStartInfinite() const
    {
        if (is_common_handle)
        {
            return (
                start.value->size() == 1
                && ((static_cast<unsigned char>((*start.value)[0]) == TiDB::CodecFlagBytes)
                    || (static_cast<unsigned char>((*start.value)[0]) == TiDB::CodecFlagNil)));
        }
        else
        {
            return start.int_value == RowKeyValue::INT_HANDLE_MIN_KEY.int_value;
        }
    }

    inline bool none() const { return start >= end; }

    inline RowKeyRange shrink(const RowKeyRange & other) const
    {
        return RowKeyRange(
            std::max(start, other.start),
            std::min(end, other.end),
            is_common_handle,
            rowkey_column_size);
    }

    inline RowKeyRange merge(const RowKeyRange & other) const
    {
        return RowKeyRange(
            std::min(start, other.start),
            std::max(end, other.end),
            is_common_handle,
            rowkey_column_size);
    }

    inline void setStart(const RowKeyValue & value) { start = value; }

    inline void setEnd(const RowKeyValue & value) { end = value; }

    inline bool intersect(const RowKeyRange & other) const { return other.start < end && start < other.end; }

    // [first, last_include]
    inline bool include(const RowKeyValueRef & first, const RowKeyValueRef & last_include) const
    {
        return check(first) && check(last_include);
    }

    /// Check whether thisRange.Start <= key
    inline bool checkStart(const RowKeyValueRef & value) const { return getStart() <= value; }

    /// Check whether key < thisRange.End
    inline bool checkEnd(const RowKeyValueRef & value) const { return value < getEnd(); }

    /// Check whether the key is included in this range.
    inline bool check(const RowKeyValueRef & value) const { return checkStart(value) && checkEnd(value); }

    inline RowKeyValueRef getStart() const { return start.toRowKeyValueRef(); }

    inline RowKeyValueRef getEnd() const { return end.toRowKeyValueRef(); }

    inline HandleRange toHandleRange() const
    {
        if (is_common_handle)
            throw Exception("Can not convert comman handle range to HandleRange");
        return {start.int_value, end.int_value};
    }

    std::pair<DecodedTiKVKeyPtr, DecodedTiKVKeyPtr> toRegionRange(TableID table_id) const
    {
        // FIXME: move this to TiKVRecordFormat.h
        WriteBufferFromOwnString ss;
        ss.write('t');
        EncodeInt64(table_id, ss);
        ss.write('_');
        ss.write('r');
        String prefix = ss.releaseStr();
        DecodedTiKVKeyPtr start_key = std::make_shared<DecodedTiKVKey>(prefix + *start.value);
        DecodedTiKVKeyPtr end_key = std::make_shared<DecodedTiKVKey>(prefix + *end.value);
        return {start_key, end_key};
    }

    /// Clip the <offset, limit> according to this range, and return the clipped <offset, limit>.
    std::pair<size_t, size_t> getPosRange(const ColumnPtr & column, size_t offset, size_t limit) const;

    static RowKeyRange fromHandleRange(const HandleRange & handle_range, bool is_common_handle = false)
    {
        if (is_common_handle)
        {
            if (handle_range.all())
            {
                return RowKeyRange(
                    RowKeyValue::COMMON_HANDLE_MIN_KEY,
                    RowKeyValue::COMMON_HANDLE_MAX_KEY,
                    /*is_common_handle=*/is_common_handle,
                    1);
            }
            WriteBufferFromOwnString ss;
            DB::EncodeUInt(static_cast<UInt8>(TiDB::CodecFlagInt), ss);
            DB::EncodeInt64(handle_range.start, ss);
            String start = ss.releaseStr();

            ss.restart();
            DB::EncodeUInt(static_cast<UInt8>(TiDB::CodecFlagInt), ss);
            DB::EncodeInt64(handle_range.end, ss);
            String end = ss.releaseStr();
            /// when handle_range.end == HandleRange::MAX, according to previous implementation, it should be +Inf
            return RowKeyRange(
                RowKeyValue(is_common_handle, std::make_shared<String>(start)),
                handle_range.end == HandleRange::MAX ? RowKeyValue::COMMON_HANDLE_MAX_KEY
                                                     : RowKeyValue(is_common_handle, std::make_shared<String>(end)),
                /*is_common_handle=*/is_common_handle,
                1);
        }
        else
        {
            if (handle_range.all())
            {
                return RowKeyRange(
                    RowKeyValue::INT_HANDLE_MIN_KEY,
                    RowKeyValue::INT_HANDLE_MAX_KEY,
                    /*is_common_handle=*/is_common_handle,
                    1);
            }
            WriteBufferFromOwnString ss;
            DB::EncodeInt64(handle_range.start, ss);
            String start = ss.releaseStr();

            ss.restart();
            DB::EncodeInt64(handle_range.end, ss);
            String end = ss.releaseStr();
            /// when handle_range.end == HandleRange::MAX, according to previous implementation, it should be +Inf
            return RowKeyRange(
                RowKeyValue(is_common_handle, std::make_shared<String>(start), handle_range.start),
                handle_range.end == HandleRange::MAX
                    ? RowKeyValue::INT_HANDLE_MAX_KEY
                    : RowKeyValue(is_common_handle, std::make_shared<String>(end), handle_range.end),
                /*is_common_handle=*/false,
                1);
        }
    }

    static RowKeyRange fromRegionRange(
        const std::shared_ptr<const RegionRangeKeys> & region_range,
        const TableID table_id,
        bool is_common_handle,
        size_t rowkey_column_size)
    {
        return fromRegionRange(
            region_range->rawKeys(),
            region_range->getMappedTableID(),
            table_id,
            is_common_handle,
            rowkey_column_size);
    }
    static RowKeyRange fromRegionRange(
        const std::pair<DecodedTiKVKeyPtr, DecodedTiKVKeyPtr> & raw_keys,
        const TableID table_id_in_raw_key,
        const TableID table_id,
        bool is_common_handle,
        size_t rowkey_column_size)
    {
        if (likely(table_id_in_raw_key == table_id))
        {
            auto & start_key = *raw_keys.first;
            auto & end_key = *raw_keys.second;
            auto keyspace_id = start_key.getKeyspaceID();
            const auto & table_range_min_max = getTableMinMaxData(keyspace_id, table_id, is_common_handle);
            RowKeyValue start_value, end_value;
            if (start_key <= *table_range_min_max.min)
            {
                if (is_common_handle)
                    start_value = RowKeyValue::COMMON_HANDLE_MIN_KEY;
                else
                    start_value = RowKeyValue::INT_HANDLE_MIN_KEY;
            }
            else
            {
                start_value = RowKeyValue(
                    is_common_handle,
                    std::make_shared<std::string>(RecordKVFormat::getRawTiDBPKView(start_key)));
            }
            if (end_key >= *table_range_min_max.max)
            {
                if (is_common_handle)
                    end_value = RowKeyValue::COMMON_HANDLE_MAX_KEY;
                else
                    end_value = RowKeyValue::INT_HANDLE_MAX_KEY;
            }
            else
                end_value = RowKeyValue(
                    is_common_handle,
                    std::make_shared<std::string>(RecordKVFormat::getRawTiDBPKView(end_key)));
            return RowKeyRange(start_value, end_value, is_common_handle, rowkey_column_size);
        }
        else
        {
            /// if table id is not the same, just return none range
            /// maybe should throw exception since it should not happen
            return newNone(is_common_handle, rowkey_column_size);
        }
    }

    // Format as a string
    String toString() const;

    // Format as a hex string for debugging. The value will be converted to '?' if redact-log is on
    String toDebugString() const;

    bool operator==(const RowKeyRange & rhs) const { return rhs.start == start && rhs.end == end; }
    bool operator!=(const RowKeyRange & rhs) const { return !(*this == rhs); }
}; // struct RowKeyRange

// Format as a hex string for debugging. The value will be converted to '?' if redact-log is on
inline String toDebugString(const RowKeyRanges & ranges)
{
    String s = "{";
    for (const auto & r : ranges)
    {
        s += r.toDebugString() + ",";
    }
    if (!ranges.empty())
        s.erase(s.size() - 1);
    s += "}";
    return s;
}

inline RowKeyRange mergeRanges(const RowKeyRanges & ranges, bool is_common_handle, size_t rowkey_column_size)
{
    auto range = RowKeyRange::newNone(is_common_handle, rowkey_column_size);
    for (const auto & r : ranges)
    {
        range.start = std::min(range.start, r.start);
        range.end = std::max(range.end, r.end);
    }
    return range;
}

} // namespace DB::DM
