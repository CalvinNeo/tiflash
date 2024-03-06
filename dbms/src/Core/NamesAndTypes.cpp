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

#include <Common/FmtUtils.h>
#include <Core/NamesAndTypes.h>
#include <DataTypes/DataTypeFactory.h>
#include <IO/Buffer/ReadBuffer.h>
#include <IO/Buffer/ReadBufferFromString.h>
#include <IO/Buffer/WriteBuffer.h>
#include <IO/Buffer/WriteBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>

#include <sparsehash/dense_hash_map>


namespace DB
{
namespace ErrorCodes
{
extern const int THERE_IS_NO_COLUMN;
}

String dumpJsonStructure(const NamesAndTypes & names_and_types)
{
    FmtBuffer bf;
    bf.append("[");
    bf.joinStr(
        names_and_types.cbegin(),
        names_and_types.cend(),
        [](const auto & name_and_type, FmtBuffer & fb) {
            fb.fmtAppend(
                R"json({{"name":"{}","type":{}}})json",
                name_and_type.name,
                (name_and_type.type ? "\"" + name_and_type.type->getName() + "\"" : "null"));
        },
        ", ");
    bf.append("]");
    return bf.toString();
}

Names toNames(const NamesAndTypes & names_and_types)
{
    Names names;
    names.reserve(names_and_types.size());
    for (const auto & name_and_type : names_and_types)
        names.push_back(name_and_type.name);
    return names;
}

void NamesAndTypesList::readText(ReadBuffer & buf)
{
    const DataTypeFactory & data_type_factory = DataTypeFactory::instance();

    assertString("columns format version: 1\n", buf);
    size_t count;
    DB::readText(count, buf);
    assertString(" columns:\n", buf);
    resize(count);
    for (NameAndTypePair & it : *this)
    {
        readBackQuotedStringWithSQLStyle(it.name, buf);
        assertChar(' ', buf);
        String type_name;
        readString(type_name, buf);
        it.type = data_type_factory.get(type_name);
        assertChar('\n', buf);
    }
}

void NamesAndTypesList::writeText(WriteBuffer & buf) const
{
    writeString("columns format version: 1\n", buf);
    DB::writeText(size(), buf);
    writeString(" columns:\n", buf);
    for (const auto & it : *this)
    {
        writeBackQuotedString(it.name, buf);
        writeChar(' ', buf);
        writeString(it.type->getName(), buf);
        writeChar('\n', buf);
    }
}

String NamesAndTypesList::toString() const
{
    WriteBufferFromOwnString out;
    writeText(out);
    return out.str();
}

NamesAndTypesList NamesAndTypesList::parse(const String & s)
{
    ReadBufferFromString in(s);
    NamesAndTypesList res;
    res.readText(in);
    assertEOF(in);
    return res;
}

bool NamesAndTypesList::isSubsetOf(const NamesAndTypesList & rhs) const
{
    NamesAndTypes vector(rhs.begin(), rhs.end());
    vector.insert(vector.end(), begin(), end());
    std::sort(vector.begin(), vector.end());
    return std::unique(vector.begin(), vector.end()) == vector.begin() + rhs.size();
}

size_t NamesAndTypesList::sizeOfDifference(const NamesAndTypesList & rhs) const
{
    NamesAndTypes vector(rhs.begin(), rhs.end());
    vector.insert(vector.end(), begin(), end());
    std::sort(vector.begin(), vector.end());
    return (std::unique(vector.begin(), vector.end()) - vector.begin()) * 2 - size() - rhs.size();
}

Names NamesAndTypesList::getNames() const
{
    Names res;
    res.reserve(size());
    for (const NameAndTypePair & column : *this)
        res.push_back(column.name);
    return res;
}

DataTypes NamesAndTypesList::getTypes() const
{
    DataTypes res;
    res.reserve(size());
    for (const NameAndTypePair & column : *this)
        res.push_back(column.type);
    return res;
}

NamesAndTypesList NamesAndTypesList::filter(const NameSet & names) const
{
    NamesAndTypesList res;
    for (const NameAndTypePair & column : *this)
    {
        if (names.count(column.name))
            res.push_back(column);
    }
    return res;
}

NamesAndTypesList NamesAndTypesList::filter(const Names & names) const
{
    return filter(NameSet(names.begin(), names.end()));
}

NamesAndTypesList NamesAndTypesList::addTypes(const Names & names) const
{
    /// NOTE It's better to make a map in `IStorage` than to create it here every time again.
    google::dense_hash_map<StringRef, const DataTypePtr *, StringRefHash> types;
    types.set_empty_key(StringRef());

    for (const NameAndTypePair & column : *this)
        types[column.name] = &column.type;

    NamesAndTypesList res;
    for (const String & name : names)
    {
        auto it = types.find(name);
        if (it == types.end())
            throw Exception("No column " + name, ErrorCodes::THERE_IS_NO_COLUMN);
        res.emplace_back(name, *it->second);
    }
    return res;
}

bool NamesAndTypesList::contains(const String & name) const
{
    for (const NameAndTypePair & column : *this)
    {
        if (column.name == name)
            return true;
    }
    return false;
}

} // namespace DB
