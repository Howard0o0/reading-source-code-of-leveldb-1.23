// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Thread-safe (provides internal synchronization)

#ifndef STORAGE_LEVELDB_DB_TABLE_CACHE_H_
#define STORAGE_LEVELDB_DB_TABLE_CACHE_H_

#include "db/dbformat.h"
#include <cstdint>
#include <string>

#include "leveldb/cache.h"
#include "leveldb/table.h"

#include "port/port.h"

namespace leveldb {

class Env;

// TableCache 在 LevelDB 中的作用是管理和缓存 SST(Sorted String Tables)的读取。
// 构造时接受一个 entries 参数，用于指定最大的缓存 SST 数量。当缓存的 SST 数量超过
// 这个限制时，TableCache 会根据某种策略（如最近最少使用，LRU）从 Cache 里移除一些
// SST。为了提高读取效率，TableCache 会缓存已打开的 SST。这样，对同一 SST 的多次读
// 取操作就不需要每次都打开文件。
class TableCache {
   public:
    // entries: 最大缓存的 Table 数量
    TableCache(const std::string& dbname, const Options& options, int entries);
    ~TableCache();

    // Return an iterator for the specified file number (the corresponding
    // file length must be exactly "file_size" bytes).  If "tableptr" is
    // non-null, also sets "*tableptr" to point to the Table object
    // underlying the returned iterator, or to nullptr if no Table object
    // underlies the returned iterator.  The returned "*tableptr" object is
    // owned by the cache and should not be deleted, and is valid for as long as
    // the returned iterator is live.
    // 
    // 返回一个指定 SST 的迭代器，用于遍历 SST 中的键值对。
    Iterator* NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                          Table** tableptr = nullptr);

    // If a seek to internal key "k" in specified file finds an entry,
    // call (*handle_result)(arg, found_key, found_value).
    //
    // 从指定 SST 中查找某个 Key。如果这个 Key 找到了，则调用 handle_result 函数。
    Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size, const Slice& k,
               void* arg, void (*handle_result)(void*, const Slice&, const Slice&));

    // Evict any entry for the specified file number
    //
    // 将某个 SST 从 TableCache 中移除。
    void Evict(uint64_t file_number);

   private:
    Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);

    Env* const env_;
    const std::string dbname_;
    const Options& options_;
    Cache* cache_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_TABLE_CACHE_H_
