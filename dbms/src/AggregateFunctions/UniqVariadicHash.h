#pragma once

#include <city.h>
#include <Core/Defines.h>
#include <Common/SipHash.h>
#include <Columns/ColumnTuple.h>

namespace DB
{

/** Hashes a set of arguments to the aggregate function
  *  to calculate the number of unique values
  *  and adds them to the set.
  *
  * Four options (2 x 2)
  *
  * - for approximate calculation, uses a non-cryptographic 64-bit hash function;
  * - for an accurate calculation, uses a cryptographic 128-bit hash function;
  *
  * - for several arguments passed in the usual way;
  * - for one argument-tuple.
  */

template <bool exact, bool for_tuple>
struct UniqVariadicHash;


template <>
struct UniqVariadicHash<false, false>
{
    static inline UInt64 apply(size_t num_args, const IColumn ** columns, size_t row_num)
    {
        UInt64 hash;

        const IColumn ** column = columns;
        const IColumn ** columns_end = column + num_args;

        {
            StringRef value = (*column)->getDataAt(row_num);
            hash = CityHash_v1_0_2::CityHash64(value.data, value.size);
            ++column;
        }

        while (column < columns_end)
        {
            StringRef value = (*column)->getDataAt(row_num);
            hash = CityHash_v1_0_2::Hash128to64(CityHash_v1_0_2::uint128(CityHash_v1_0_2::CityHash64(value.data, value.size), hash));
            ++column;
        }

        return hash;
    }
};

template <>
struct UniqVariadicHash<false, true>
{
    static inline UInt64 apply(size_t num_args, const IColumn ** columns, size_t row_num)
    {
        UInt64 hash;

        const Columns & tuple_columns = static_cast<const ColumnTuple *>(columns[0])->getColumns();

        const ColumnPtr * column = tuple_columns.data();
        const ColumnPtr * columns_end = column + num_args;

        {
            StringRef value = column->get()->getDataAt(row_num);
            hash = CityHash_v1_0_2::CityHash64(value.data, value.size);
            ++column;
        }

        while (column < columns_end)
        {
            StringRef value = column->get()->getDataAt(row_num);
            hash = CityHash_v1_0_2::Hash128to64(CityHash_v1_0_2::uint128(CityHash_v1_0_2::CityHash64(value.data, value.size), hash));
            ++column;
        }

        return hash;
    }
};

template <>
struct UniqVariadicHash<true, false>
{
    static inline UInt128 apply(size_t num_args, const IColumn ** columns, size_t row_num)
    {
        const IColumn ** column = columns;
        const IColumn ** columns_end = column + num_args;

        SipHash hash;

        while (column < columns_end)
        {
            (*column)->updateHashWithValue(row_num, hash);
            ++column;
        }

        UInt128 key;
        hash.get128(key.low, key.high);
        return key;
    }
};

template <>
struct UniqVariadicHash<true, true>
{
    static inline UInt128 apply(size_t num_args, const IColumn ** columns, size_t row_num)
    {
        const Columns & tuple_columns = static_cast<const ColumnTuple *>(columns[0])->getColumns();

        const ColumnPtr * column = tuple_columns.data();
        const ColumnPtr * columns_end = column + num_args;

        SipHash hash;

        while (column < columns_end)
        {
            (*column)->updateHashWithValue(row_num, hash);
            ++column;
        }

        UInt128 key;
        hash.get128(key.low, key.high);
        return key;
    }
};

}
