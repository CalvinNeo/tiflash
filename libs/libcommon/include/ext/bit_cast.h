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

#include <string.h>

#include <algorithm>
#include <type_traits>


namespace ext
{
/** \brief Returns value `from` converted to type `To` while retaining bit representation.
  *    `To` and `From` must satisfy `CopyConstructible`.
  */
template <typename To, typename From>
std::decay_t<To> bit_cast(const From & from)
{
    To res{};

#pragma GCC diagnostic ignored "-Wpragmas"
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif
    memcpy(&res, &from, std::min(sizeof(res), sizeof(from)));
    return res;
};

/** \brief Returns value `from` converted to type `To` while retaining bit representation.
  *    `To` and `From` must satisfy `CopyConstructible`.
  */
template <typename To, typename From>
std::decay_t<To> safe_bit_cast(const From & from)
{
    static_assert(sizeof(To) == sizeof(From), "bit cast on types of different width");
    return ext::bit_cast<To, From>(from);
};
} // namespace ext
