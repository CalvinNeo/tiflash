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

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


namespace DB
{
using Names = std::vector<std::string>;
using NameSet = std::unordered_set<std::string>;
using NameToNameMap = std::unordered_map<std::string, std::string>;

/* TODO: only support a small number of names now
 */
class OrderedNameSet : public Names
{
public:
    bool has(const std::string & name) const
    {
        for (const auto & it : *this)
            if (it == name)
                return true;
        return false;
    }
};

} // namespace DB
