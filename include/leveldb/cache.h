// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Cache is an interface that maps keys to values.  It has internal
// synchronization and may be safely accessed concurrently from
// multiple threads.  It may automatically evict entries to make room
// for new entries.  Values have a specified charge against the cache
// capacity.  For example, a cache where the values are variable
// length strings, may use the length of the string as the charge for
// the string.
//
// A builtin cache implementation with a least-recently-used eviction
// policy is provided.  Clients may use their own implementations if
// they want something more sophisticated (like scan-resistance, a
// custom eviction policy, variable cache sizing, etc.)

#ifndef STORAGE_LEVELDB_INCLUDE_CACHE_H_
#define STORAGE_LEVELDB_INCLUDE_CACHE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/slice.h"

namespace leveldb {

class LEVELDB_EXPORT Cache;

// Create a new cache with a fixed size capacity.  This implementation
// of Cache uses a least-recently-used eviction policy.
LEVELDB_EXPORT Cache* NewLRUCache(size_t capacity);

class LEVELDB_EXPORT Cache {
   public:
    Cache() = default;

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    // Destroys all existing entries by calling the "deleter"
    // function that was passed to the constructor.
    //
    // 用户调用 Insert() 方法插入一个缓存项时，会同时传入一个 deleter。
    // 当 Cache 析构时，会调用所有缓存项的 deleter 来对 Cache 里的
    // 所有缓存项进行清理。
    virtual ~Cache();

    // Opaque handle to an entry stored in the cache.
    //
    // 声明了一个抽象的 Handle 类型，用于表示 Cache 中的一个缓存项。
    struct Handle {};

    // Insert a mapping from key->value into the cache and assign it
    // the specified charge against the total cache capacity.
    //
    // Returns a handle that corresponds to the mapping.  The caller
    // must call this->Release(handle) when the returned mapping is no
    // longer needed.
    //
    // When the inserted entry is no longer needed, the key and
    // value will be passed to "deleter".
    //
    // 这个方法用于向 Cache 中插入一对 Key-Value，Cache 的实现类会在内部
    // 基于这对 Key-Value 生成一个 Handle 对象，将该 Handle 插入到 Cache
    // 中，并把这个 Handle 的指针返回给用户。
    //
    // 返回 Handle 指针，相当于这个 Cache 缓存项的引用计数加一，即使另外一个线程
    // 将这个缓存项从 Cache 中移除了，该缓存项还是会保留在内存中。
    // 用户需要在使用完这个缓存项后，调用 Release() 方法来将该缓存项的引用计数减一。
    // 可以通过以下例子来理解:
    //      Cache* cache = NewLRUCache(100);
    //      Handle* handle = cache->Insert("key", "value", 5, nullptr);
    //      cache->Erase("key"); // 即使这里将缓存项从 Cache 中移除了，但该缓存项还是会保留在内存中
    //      cache->Value(handle); // 此时还是可以通过 handle 获取到缓存项的 Value
    //      cache->Release(handle); // 这里将缓存项的引用计数减一，如果引用计数为 0，那么该缓存项会被销毁。
    //
    // charge 参数表示这个缓存项的大小，因为缓存项里只存储了 value 的指针，所以计算出
    // 该缓存项的大小，需要用户告知 Cache。
    virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                           void (*deleter)(const Slice& key, void* value)) = 0;

    // If the cache has no mapping for "key", returns nullptr.
    //
    // Else return a handle that corresponds to the mapping.  The caller
    // must call this->Release(handle) when the returned mapping is no
    // longer needed.
    // 
    // 通过 key 查找 Cache 中的缓存项:
    //      - 如果找到了，返回该缓存项的 Handle 指针。
    //      - 同时将该缓存项的引用计数加一。如果没有找到，返回 nullptr。
    virtual Handle* Lookup(const Slice& key) = 0;

    // Release a mapping returned by a previous Lookup().
    // REQUIRES: handle must not have been released yet.
    // REQUIRES: handle must have been returned by a method on *this.
    //
    // 将缓存项的引用计数减一，如果引用计数为 0，那么该缓存项会被销毁。
    virtual void Release(Handle* handle) = 0;

    // Return the value encapsulated in a handle returned by a
    // successful Lookup().
    // REQUIRES: handle must not have been released yet.
    // REQUIRES: handle must have been returned by a method on *this.
    //
    // 返回缓存项的 Value。
    virtual void* Value(Handle* handle) = 0;

    // If the cache contains entry for key, erase it.  Note that the
    // underlying entry will be kept around until all existing handles
    // to it have been released.
    //
    // 指定一个 key，从 Cache 中移除一个缓存项。
    // 如果该缓存项的引用计数为 0，那么该缓存项会被销毁。
    virtual void Erase(const Slice& key) = 0;

    // Return a new numeric id.  May be used by multiple clients who are
    // sharing the same cache to partition the key space.  Typically the
    // client will allocate a new id at startup and prepend the id to
    // its cache keys.
    //
    // 返回一个新的数字，作为该 Cache 的 ID。
    virtual uint64_t NewId() = 0;

    // Remove all cache entries that are not actively in use. Memory-constrained
    // applications may wish to call this method to reduce memory usage.
    // Default implementation of Prune() does nothing.  Subclasses are strongly
    // encouraged to override the default implementation.  A future release of
    // leveldb may change Prune() to a pure abstract method.
    //
    // 移除 Cache 中所有没有正在被使用的缓存项，也就是引用计数为 1 的那些。
    // 比如在一些内存紧张的情况下，客户端可能会希望把 Cache 里没有正在被使用的缓存项移除，
    // 腾出一些内存空间。
    virtual void Prune() {}

    // Return an estimate of the combined charges of all elements stored in the
    // cache.
    // 
    // 计算 Cache 中所有缓存项的大小之和。
    virtual size_t TotalCharge() const = 0;

   private:
    void LRU_Remove(Handle* e);
    void LRU_Append(Handle* e);
    void Unref(Handle* e);

    struct Rep;
    Rep* rep_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_CACHE_H_
