// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"

#include "leveldb/env.h"
#include "leveldb/table.h"

#include "util/coding.h"

namespace leveldb {

struct TableAndFile {
    RandomAccessFile* file;
    Table* table;
};

static void DeleteEntry(const Slice& key, void* value) {
    TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
    delete tf->table;
    delete tf->file;
    delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
    Cache* cache = reinterpret_cast<Cache*>(arg1);
    Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
    cache->Release(h);
}

TableCache::TableCache(const std::string& dbname, const Options& options, int entries)
    : env_(options.env), dbname_(dbname), options_(options), cache_(NewLRUCache(entries)) {}

TableCache::~TableCache() { delete cache_; }

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle) {
    Status s;

    // 将 file_number 编码为 fixed64，作为 key 到
    // cache_ 中查找 handle。
    char buf[sizeof(file_number)];
    EncodeFixed64(buf, file_number);
    Slice key(buf, sizeof(buf));
    *handle = cache_->Lookup(key);

    // 如果 cache_ 中木有找到，就打开该 SST 文件，并将其添加到 cache_ 中。
    if (*handle == nullptr) {
        // 根据 file_number 构造出 SST 的文件名。
        // 早期版本的 LevelDB 使用的是 .sst 后缀，后来改为了 .ldb。
        // 为了兼容这两种命名方式，这里会尝试两种后缀。
        // TableFileName() 会构建 .ldb 后缀的 SST 文件名，
        // SSTTableFileName() 会构建 .sst 后缀的 SST 文件名。
        std::string fname = TableFileName(dbname_, file_number);
        RandomAccessFile* file = nullptr;
        Table* table = nullptr;
        s = env_->NewRandomAccessFile(fname, &file);
        if (!s.ok()) {
            std::string old_fname = SSTTableFileName(dbname_, file_number);
            if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
                s = Status::OK();
            }
        }

        // SST 文件打开后，通过 Table::Open 创建一个 Table 对象。
        if (s.ok()) {
            s = Table::Open(options_, file, file_size, &table);
        }

        if (!s.ok()) {
            // 如果创建 Table 对象失败，就关闭 SST 文件的句柄。
            assert(table == nullptr);
            delete file;
            // We do not cache error results so that if the error is transient,
            // or somebody repairs the file, we recover automatically.
        } else {
            // Table 对象创建成功，将其添加到 cache_ 中。
            TableAndFile* tf = new TableAndFile;
            tf->file = file;
            tf->table = table;
            *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
        }
    }
    return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options, uint64_t file_number,
                                  uint64_t file_size, Table** tableptr) {
    if (tableptr != nullptr) {
        *tableptr = nullptr;
    }

    // 在 Cache 中找到指定的 SST。
    // 如果目标 SST 不在缓存中，它会打开文件并将其添加到 Cache。
    // handle 指向 Cache 中的 SST Item。
    Cache::Handle* handle = nullptr;
    Status s = FindTable(file_number, file_size, &handle);
    if (!s.ok()) {
        return NewErrorIterator(s);
    }

    // 通过 handle 在 cache 中获取 SST 对应的 Table 对象。
    Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    // 调用 Table::NewIterator() 方法创建该 SST 的 Iterator。
    Iterator* result = table->NewIterator(options);
    result->RegisterCleanup(&UnrefEntry, cache_, handle);
    if (tableptr != nullptr) {
        *tableptr = table;
    }
    return result;
}

Status TableCache::Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                       const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&, const Slice&)) {
    
    // 在 Cache 中找到指定的 SST。
    // 如果目标 SST 不在缓存中，它会打开文件并将其添加到 Cache。
    // handle 指向 Cache 中的 SST Item。
    Cache::Handle* handle = nullptr;
    Status s = FindTable(file_number, file_size, &handle);
    if (s.ok()) {
        // 通过 handle 在 cache 中获取 SST 对应的 Table 对象。
        Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
        // 调用 Table::InternalGet() 方法从 SST 中查找指定的 key。
        s = t->InternalGet(options, k, arg, handle_result);
        cache_->Release(handle);
    }
    return s;
}

void TableCache::Evict(uint64_t file_number) {
    // 将 file_number 编码为 fixed64，
    // 作为 cache_ 中的 key，将该 key 从
    // cache_ 中移除。
    char buf[sizeof(file_number)];
    EncodeFixed64(buf, file_number);
    cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
