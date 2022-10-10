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

#include <Common/Logger.h>
#include <Common/nocopyable.h>
#include <Storages/Transaction/ProxyFFI.h>

namespace DB
{
class SSTReader
{
public:
    virtual bool remained() const;
    virtual BaseBuffView keyView() const;
    virtual BaseBuffView valueView() const;
    virtual void next();

    DISALLOW_COPY_AND_MOVE(SSTReader);
    SSTReader(const TiFlashRaftProxyHelper * proxy_helper_, SSTView view);
    // Should only be used when SSTReader is a base class.
    SSTReader();
    virtual ~SSTReader();

private:
    const TiFlashRaftProxyHelper * proxy_helper;
    SSTReaderPtr inner;
    ColumnFamilyType type;
    bool inited;
};

/// MultiSSTReader helps when there are multiple sst files in a column family.
/// It is derived from virtual class SSTReader, so it can be holded in a SSTReaderPtr.
/// It also maintains instance of `R` which is normaly SSTReader(and MockSSTReader in tests),
/// to read a single sst file.
/// An `Initer` function is need to create a instance of `R` from a instance of `E`,
/// which is usually path of the SST file.
/// We introduce `R` and `E` to make this class testable.
template <typename R, typename E>
class MultiSSTReader : public SSTReader
{
public:
    using Initer = std::function<std::unique_ptr<R>(const TiFlashRaftProxyHelper *, E)>;

    DISALLOW_COPY_AND_MOVE(MultiSSTReader);

    bool remained() const override
    {
        this->maybe_next_reader();
        return mono->remained();
    }
    BaseBuffView keyView() const override
    {
        return mono->keyView();
    }
    BaseBuffView valueView() const override
    {
        return mono->valueView();
    }
    void next() override
    {
        this->maybe_next_reader();
        mono->next();
    }
    void maybe_next_reader() const
    {
        if (!mono->remained())
        {
            current++;
            if (current < args.size())
            {
                // We delete current mono, iif:
                // 1. We can switch to next reader mono;
                // 2. We must switch to next reader mono;
                // We don't drop if mono is the last instance for safety,
                // and it will be dropped as MultiSSTReader is dropped.
                LOG_FMT_INFO(log, "Swithch to {}", buffToStrView(args[current].path));
                mono = initer(proxy_helper, args[current]);
            }
        }
    }

    MultiSSTReader(const TiFlashRaftProxyHelper * proxy_helper_, ColumnFamilyType type_, Initer initer_, std::vector<E> args_)
        : proxy_helper(proxy_helper_)
        , type(type_)
        , initer(initer_)
        , args(args_)
        , current(0)
    {
        assert(args.size() > 0);
        log = &Poco::Logger::get("MultiSSTReader");
        LOG_FMT_INFO(log, "Init with {}", buffToStrView(args[current].path));
        mono = initer(proxy_helper, args[current]);
    }

    ~MultiSSTReader()
    {
        // The last sst reader will be dropped with inner.
    }

private:
    Poco::Logger * log;
    mutable std::unique_ptr<R> mono;
    const TiFlashRaftProxyHelper * proxy_helper;
    ColumnFamilyType type;
    Initer initer;
    std::vector<E> args;
    mutable size_t current;
};

} // namespace DB
