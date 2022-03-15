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

#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsRandom.h>

namespace DB
{
namespace detail
{
void seed(LinearCongruentialGenerator & generator, intptr_t additional_seed)
{
    generator.seed(intHash64(randomSeed() ^ intHash64(additional_seed)));
}
} // namespace detail


void registerFunctionsRandom(FunctionFactory & factory)
{
    factory.registerFunction<FunctionRand>();
    factory.registerFunction<FunctionRand64>();
    factory.registerFunction<FunctionRandConstant>();
}

} // namespace DB
