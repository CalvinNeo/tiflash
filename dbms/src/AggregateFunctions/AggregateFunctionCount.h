#pragma once

#include <IO/VarInt.h>

#include <array>
#include <DataTypes/DataTypesNumber.h>
#include <Columns/ColumnsCommon.h>
#include <Columns/ColumnNullable.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <IO/WriteHelpers.h>


namespace DB
{

struct AggregateFunctionCountData
{
    UInt64 count = 0;
};

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}


/// Simply count number of calls.
class AggregateFunctionCount final : public IAggregateFunctionDataHelper<AggregateFunctionCountData, AggregateFunctionCount>
{
public:
    String getName() const override { return "count"; }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }

    void add(AggregateDataPtr __restrict place, const IColumn **, size_t, Arena *) const override
    {
        ++data(place).count;
    }

    void addBatchSinglePlace(
        size_t batch_size, AggregateDataPtr place, const IColumn ** columns, Arena *, ssize_t if_argument_pos) const override
    {
        if (if_argument_pos >= 0)
        {
            const auto & flags = assert_cast<const ColumnUInt8 &>(*columns[if_argument_pos]).getData();
            data(place).count += countBytesInFilter(flags);
        }
        else
        {
            data(place).count += batch_size;
        }
    }

    void addBatchSinglePlaceNotNull(
        size_t batch_size,
        AggregateDataPtr place,
        const IColumn ** columns,
        const UInt8 * null_map,
        Arena *,
        ssize_t if_argument_pos) const override
    {
        if (if_argument_pos >= 0)
        {
            const auto & flags = assert_cast<const ColumnUInt8 &>(*columns[if_argument_pos]).getData();
            data(place).count += countBytesInFilterWithNull(flags, null_map);
        }
        else
        {
            data(place).count += batch_size - countBytesInFilter(null_map, batch_size);
        }
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        data(place).count += data(rhs).count;
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf) const override
    {
        writeVarUInt(data(place).count, buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, Arena *) const override
    {
        readVarUInt(data(place).count, buf);
    }

    void insertResultInto(ConstAggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        static_cast<ColumnUInt64 &>(to).getData().push_back(data(place).count);
    }

    /// May be used for optimization.
    void addDelta(AggregateDataPtr __restrict place, UInt64 x) const
    {
        data(place).count += x;
    }

    const char * getHeaderFilePath() const override { return __FILE__; }
};


/// Simply count number of not-NULL values.
class AggregateFunctionCountNotNullUnary final : public IAggregateFunctionDataHelper<AggregateFunctionCountData, AggregateFunctionCountNotNullUnary>
{
public:
    AggregateFunctionCountNotNullUnary(const DataTypePtr & argument)
    {
        if (!argument->isNullable())
            throw Exception("Logical error: not Nullable data type passed to AggregateFunctionCountNotNullUnary", ErrorCodes::LOGICAL_ERROR);
    }

    String getName() const override { return "count"; }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        data(place).count += !static_cast<const ColumnNullable &>(*columns[0]).isNullAt(row_num);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        data(place).count += data(rhs).count;
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf) const override
    {
        writeVarUInt(data(place).count, buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, Arena *) const override
    {
        readVarUInt(data(place).count, buf);
    }

    void insertResultInto(ConstAggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        static_cast<ColumnUInt64 &>(to).getData().push_back(data(place).count);
    }

    const char * getHeaderFilePath() const override { return __FILE__; }
};


/// Count number of calls where all arguments are not NULL.
class AggregateFunctionCountNotNullVariadic final : public IAggregateFunctionDataHelper<AggregateFunctionCountData, AggregateFunctionCountNotNullVariadic>
{
public:
    AggregateFunctionCountNotNullVariadic(const DataTypes & arguments)
    {
        number_of_arguments = arguments.size();

        if (number_of_arguments == 1)
            throw Exception("Logical error: single argument is passed to AggregateFunctionCountNotNullVariadic", ErrorCodes::LOGICAL_ERROR);

        if (number_of_arguments > MAX_ARGS)
            throw Exception("Maximum number of arguments for aggregate function with Nullable types is " + toString(size_t(MAX_ARGS)),
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        for (size_t i = 0; i < number_of_arguments; ++i)
            is_nullable[i] = arguments[i]->isNullable();
    }

    String getName() const override { return "count"; }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        for (size_t i = 0; i < number_of_arguments; ++i)
            if (is_nullable[i] && static_cast<const ColumnNullable &>(*columns[i]).isNullAt(row_num))
                return;

        ++data(place).count;
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        data(place).count += data(rhs).count;
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf) const override
    {
        writeVarUInt(data(place).count, buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, Arena *) const override
    {
        readVarUInt(data(place).count, buf);
    }

    void insertResultInto(ConstAggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        static_cast<ColumnUInt64 &>(to).getData().push_back(data(place).count);
    }

    const char * getHeaderFilePath() const override { return __FILE__; }

private:
    enum { MAX_ARGS = 8 };
    size_t number_of_arguments = 0;
    std::array<char, MAX_ARGS> is_nullable;    /// Plain array is better than std::vector due to one indirection less.
};

}
