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

#include <Functions/FunctionBinaryArithmetic.h>

namespace DB
{
namespace
{
template <typename A, typename B, bool existDecimal = IsDecimal<A> || IsDecimal<B>>
struct BitXorImpl;

template <typename A, typename B>
struct BitXorImpl<A, B, false>
{
    using ResultType = typename NumberTraits::ResultOfBit<A, B>::Type;

    template <typename Result = ResultType>
    static Result apply(A a, B b)
    {
        return static_cast<Result>(a) ^ static_cast<Result>(b);
    }
    template <typename Result = ResultType>
    static Result apply(A, B, UInt8 &)
    {
        throw Exception("Should not reach here");
    }
};

template <typename A, typename B>
struct BitXorImpl<A, B, true>
{
    using ResultType = If<std::is_unsigned_v<A> || std::is_unsigned_v<B>, uint64_t, int64_t>;

    template <typename Result = ResultType>
    static Result apply(A a, B b)
    {
        Result x, y;
        if constexpr (IsDecimal<A>)
        {
            x = static_cast<Result>(a.value);
        }
        else
        {
            x = static_cast<Result>(a);
        }
        if constexpr (IsDecimal<B>)
        {
            y = static_cast<Result>(b.value);
        }
        else
        {
            y = static_cast<Result>(b);
        }
        return BitXorImpl<Result, Result>::apply(x, y);
    }
    template <typename Result = ResultType>
    static Result apply(A, B, UInt8 &)
    {
        throw Exception("Should not reach here");
    }
};

// clang-format off
struct NameBitXor               { static constexpr auto name = "bitXor"; };
// clang-format on

template <typename A, typename B>
using BitXorImpl_t = BitXorImpl<A, B>;
using FunctionBitXor = FunctionBinaryArithmetic<BitXorImpl_t, NameBitXor>;

} // namespace

void registerFunctionBitXor(FunctionFactory & factory)
{
    factory.registerFunction<FunctionBitXor>();
}

} // namespace DB