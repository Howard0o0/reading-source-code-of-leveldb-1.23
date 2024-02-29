// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {}

namespace {

// LRU cache implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
//
// The cache keeps two linked lists of items in the cache.  All items in the
// cache are in one list or the other, and never both.  Items still referenced
// by clients but erased from the cache are in neither list.  The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.)
// - LRU:  contains the items not currently referenced by clients, in LRU order
// Elements are moved between these lists by the Ref() and Unref() methods,
// when they detect an element in the cache acquiring or losing its only
// external reference.

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
struct LRUHandle {
    void* value;
    void (*deleter)(const Slice&, void* value);
    LRUHandle* next_hash; // 如果两个缓存项的 hash 值相同，那么它们会被放到一个链表中，next_hash 就是链表中的下一个缓存项
    LRUHandle* next; // LRU 链表中的下一个(更新的)缓存项
    LRUHandle* prev; // LRU 链表中的上一个(更旧的)缓存项
    size_t charge;  // 该缓存项的大小
    size_t key_length; // key 的长度
    bool in_cache;     // 该缓存项是否还在 Cache 中
    uint32_t refs;     // 引用次数 
    uint32_t hash;     // key 的 hash 值
    char key_data[1];  // key

    Slice key() const {
        // next_ is only equal to this if the LRU handle is the list head of an
        // empty list. List heads never have meaningful keys.
        assert(next != this);

        return Slice(key_data, key_length);
    }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
//
// LevelDB 实现了一个简单的 Hash 表，以 {key, hash} 作为 Hash 表的 Key，
// 以 LRUHandle 作为 Hash 表的 Value。
class HandleTable {
   public:
    HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
    ~HandleTable() { delete[] list_; }

    LRUHandle* Lookup(const Slice& key, uint32_t hash) { return *FindPointer(key, hash); }

    // 插入一个新的 LRUHandle, 返回一个和这个新 LRUHandle 相同 Key 的老 LRUHandle，
    // 如果存在的话。
    LRUHandle* Insert(LRUHandle* h) {
        // 找到 key 对应的 LRUHandle* 在 Hash 表中的位置。
        // 如果哈希表中存在相同 key 的缓存项，那么返回老的 LRUHandle* 
        // 在 Hash 表中的位置。
        // 如果哈希表中不存在相同 key 的缓存项，那么返回新的 LRUHandle*
        // 需要插入到 Hash 表中的位置。
        LRUHandle** ptr = FindPointer(h->key(), h->hash);
        
        // 先把老的 LRUHandle* 保存下来，最后返回给客户端。
        LRUHandle* old = *ptr;
        // 如果 old 存在，就用新的 LRUHandle* 替换掉 old。
        h->next_hash = (old == nullptr ? nullptr : old->next_hash);
        *ptr = h;
        if (old == nullptr) {
            /// 如果 old 不存在，表示哈希表中需要新插入一个 LRUHandle*。
            // 此时需要更新哈希表的元素个数，如果元素个数超过了哈希表的长度，
            // 则需要对哈希表进行扩容。
            ++elems_;
            if (elems_ > length_) {
                // Since each cache entry is fairly large, we aim for a small
                // average linked list length (<= 1).
                Resize();
            }
        }
        return old;
    }

    LRUHandle* Remove(const Slice& key, uint32_t hash) {
        // 找到 key 对应的 LRUHandle* 在 Hash 表中的位置。
        LRUHandle** ptr = FindPointer(key, hash);
        LRUHandle* result = *ptr;
        if (result != nullptr) {
            // 如果找到了，那么需要将该 LRUHandle* 从 Hash 表中移除，
            // 并且更新哈希表的元素个数。
            *ptr = result->next_hash;
            --elems_;
        }
        return result;
    }

   private:
    // The table consists of an array of buckets where each bucket is
    // a linked list of cache entries that hash into the bucket.
    uint32_t length_;
    uint32_t elems_;
    LRUHandle** list_;

    // Return a pointer to slot that points to a cache entry that
    // matches key/hash.  If there is no such cache entry, return a
    // pointer to the trailing slot in the corresponding linked list.
    LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
        // key 的 hash 值模上哈希表的长度，得到 key 在哈希表中的位置。
        // 这个位置其实是哈希冲突链表的头节点，遍历这个冲突链表，找到
        // key 对应的 LRUHandle*。
        LRUHandle** ptr = &list_[hash & (length_ - 1)];
        while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
            ptr = &(*ptr)->next_hash;
        }
        return ptr;
    }

    void Resize() {
        // 哈希表扩容后的最小长度是 4
        uint32_t new_length = 4;
        // 将哈希表的长度以指数增长的方式扩大，
        // 一直扩大到可以容纳下哈希表里的所有
        // 元素为止。　
        while (new_length < elems_) {
            new_length *= 2;
        }

        // 创建一张新哈希表，将老哈希表里的所有元素逐一 hash 到新哈希表中。
        LRUHandle** new_list = new LRUHandle*[new_length];
        memset(new_list, 0, sizeof(new_list[0]) * new_length);
        uint32_t count = 0;
        for (uint32_t i = 0; i < length_; i++) {
            LRUHandle* h = list_[i];
            while (h != nullptr) {
                // 如果存在 hash 冲突，那么将冲突的 LRUHandle* 插入到冲突链表的尾部。
                LRUHandle* next = h->next_hash;
                uint32_t hash = h->hash;
                LRUHandle** ptr = &new_list[hash & (new_length - 1)];
                h->next_hash = *ptr;
                *ptr = h;
                h = next;
                count++;
            }
        }
        assert(elems_ == count);
        // 销毁老哈希表，用新哈希表替换掉老哈希表。
        delete[] list_;
        list_ = new_list;
        length_ = new_length;
    }
};

// A single shard of sharded cache.
class LRUCache {
   public:
    LRUCache();
    ~LRUCache();

    // Separate from constructor so caller can easily make an array of LRUCache
    //
    // 设置 Cache 的容量。
    // 当插入一条缓存项使得 Cache 的总大小超过容量时，会将最老(访问时间最早)的缓存项移除。
    void SetCapacity(size_t capacity) { capacity_ = capacity; }

    // Like Cache methods, but with an extra "hash" parameter.

    // 插入一个缓存项到 Cache 中，同时注册该缓存项的销毁回调函数。
    // key: 缓存项的 key
    // hash: key 的 hash 值，需要客户端自己计算
    // value: 缓存数据的指针
    // charge: 缓存项的大小，需要客户端自己计算，因为缓存项里只存储了缓存数据的指针
    // deleter: 缓存项的销毁回调函数
    Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value, size_t charge,
                          void (*deleter)(const Slice& key, void* value));
    
    // 根据 key 和 hash 查找缓存项。
    Cache::Handle* Lookup(const Slice& key, uint32_t hash);

    // 将缓存项的引用次数减一。
    void Release(Cache::Handle* handle);

    // 将缓存项从 Cache 中移除。
    void Erase(const Slice& key, uint32_t hash);

    // 移除 Cache 中所有没有正在被使用的缓存项，也就是引用计数为 1 的那些。
    void Prune();

    // 返回 Cache 里所有缓存项的总大小，也就是 Cache 的占用的内存空间。
    size_t TotalCharge() const {
        MutexLock l(&mutex_);
        return usage_;
    }

   private:
    void LRU_Remove(LRUHandle* e);
    void LRU_Append(LRUHandle* list, LRUHandle* e);
    void Ref(LRUHandle* e);
    void Unref(LRUHandle* e);
    // 将 e 从 Cache 中移除
    bool FinishErase(LRUHandle* e) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

    // Initialized before use.
    size_t capacity_;

    // mutex_ protects the following state.
    mutable port::Mutex mutex_;
    size_t usage_ GUARDED_BY(mutex_);

    // Dummy head of LRU list.
    // lru.prev is newest entry, lru.next is oldest entry.
    // Entries have refs==1 and in_cache==true.
    // 
    // LRU 链表的 Dummy Head 节点。
    // LRU 链表中的最新的缓存项是尾节点，最老的是头节点。
    // LRU 链表中存放 refs == 1 && in_cache == true 的缓存项。
    LRUHandle lru_ GUARDED_BY(mutex_);

    // Dummy head of in-use list.
    // Entries are in use by clients, and have refs >= 2 and in_cache==true.
    //
    // in_use 链表的 Dummy Head 节点。
    // in_use 链表中的缓存项是正在被客户端使用的，它们的引用次数 >= 2，in_cache==true。
    LRUHandle in_use_ GUARDED_BY(mutex_);

    // Hash table of all cache entries.
    //
    // Cache 中所有缓存项的 Hash 表，用于快速查找缓存项。
    HandleTable table_ GUARDED_BY(mutex_);
};

LRUCache::LRUCache() : capacity_(0), usage_(0) {
    // Make empty circular linked lists.
    lru_.next = &lru_;
    lru_.prev = &lru_;
    in_use_.next = &in_use_;
    in_use_.prev = &in_use_;
}

LRUCache::~LRUCache() {
    assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
    for (LRUHandle* e = lru_.next; e != &lru_;) {
        LRUHandle* next = e->next;
        assert(e->in_cache);
        e->in_cache = false;
        assert(e->refs == 1);  // Invariant of lru_ list.
        Unref(e);
        e = next;
    }
}

void LRUCache::Ref(LRUHandle* e) {
    if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
        LRU_Remove(e);
        LRU_Append(&in_use_, e);
    }
    e->refs++;
}

void LRUCache::Unref(LRUHandle* e) {
    assert(e->refs > 0);
    // 将缓存项的引用次数减一。
    e->refs--;
    if (e->refs == 0) {  // Deallocate.
        // 如果引用计数减少后为 0，调用 deleter 销毁该缓存项。
        assert(!e->in_cache);
        (*e->deleter)(e->key(), e->value);
        free(e);
    } else if (e->in_cache && e->refs == 1) {
        // No longer in use; move to lru_ list.
        //
        // 如果引用计数减少后为 1，表示该缓存项已经没有正在使用的客户端了，
        // 那么需要将该缓存项从 in_use_ 链表中移除，然后插入回 lru_ 链表中。
        LRU_Remove(e);
        LRU_Append(&lru_, e);
    }
}

void LRUCache::LRU_Remove(LRUHandle* e) {
    e->next->prev = e->prev;
    e->prev->next = e->next;
}

void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
    // Make "e" newest entry by inserting just before *list
    e->next = list;
    e->prev = list->prev;
    e->prev->next = e;
    e->next->prev = e;
}

Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
    MutexLock l(&mutex_);
    // 到 Hash 表中查找 key 对应的缓存项指针。
    LRUHandle* e = table_.Lookup(key, hash);

    // 如果找到了缓存项，那么需要将缓存项的引用次数加一，
    // 然后返回该缓存项指针。
    if (e != nullptr) {
        Ref(e);
    }
    return reinterpret_cast<Cache::Handle*>(e);
}

void LRUCache::Release(Cache::Handle* handle) {
    MutexLock l(&mutex_);
    // 将缓存项的引用次数减一。
    Unref(reinterpret_cast<LRUHandle*>(handle));
}

Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value, size_t charge,
                                void (*deleter)(const Slice& key, void* value)) {
    MutexLock l(&mutex_);

    // 构造一个 LRUHandle 节点
    LRUHandle* e = reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
    e->value = value;
    e->deleter = deleter;
    e->charge = charge;
    e->key_length = key.size();
    e->hash = hash;
    e->in_cache = false;
    // 提前把引用计数先加一，因为 Insert 结束后需要把创建出来的 LRUHandle 地址
    // 返回给客户端，客户端对该 LRUHandle 的引用需要加一。
    e->refs = 1;  // for the returned handle.
    std::memcpy(e->key_data, key.data(), key.size());

    // 如果打开数据库时配置了禁止使用 Cache，则创建出来的 Cache Capacity 就会是 0。
    if (capacity_ > 0) {
        // 这里的引用计数加一表示该 LRUHandle 在 Cache 中，是 Cache 对 LRUHandle
        // 的引用。
        e->refs++;  // for the cache's reference.
        e->in_cache = true;
        // 把 LRUHandle 节点按照 LRU 的策略插入到 in_use_ 链表中。
        LRU_Append(&in_use_, e);
        usage_ += charge;
        // 把 LRUHandle 节点插入到 Hash 表中。
        // 如果存在相同 key 的缓存项，那么`table_.Insert(e)`会返回老的缓存项。
        // 如果存在老的缓存项，那么需要将老的缓存项从 Cache 中移除。
        FinishErase(table_.Insert(e));
    } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
        // next is read by key() in an assert, so it must be initialized
        // 
        // capacity_ == 0 表示禁止使用 Cache，所以这里不需要把 LRUHandle 节点插入到
        // 链表中。
        e->next = nullptr;
    }

    // 如果插入新的 LRUHandle 节点后，Cache 的总大小超过了容量，那么需要将最老的
    // LRUHandle 节点移除，直到 Cache 的总大小不溢出容量。
    while (usage_ > capacity_ && lru_.next != &lru_) {
        // +->oldest <-> youngest <-> lru_<-+
        // +--------------------------------+
        LRUHandle* old = lru_.next;
        assert(old->refs == 1);
        bool erased = FinishErase(table_.Remove(old->key(), old->hash));
        if (!erased) {  // to avoid unused variable when compiled NDEBUG
            assert(erased);
        }
    }

    return reinterpret_cast<Cache::Handle*>(e);
}

// If e != nullptr, finish removing *e from the cache; it has already been
// removed from the hash table.  Return whether e != nullptr.
// 
// 将缓存项从 Cache 中移除。
// LRUCache::FinishErase(e) 和 LRUCache::Erase(key, hash) 的不同之处是:
//      - LRUCache::FinishErase(e) 不负责将 e 从 table_ 中移除， 
//      - LRUCache::Erase(key, hash) 负责。
bool LRUCache::FinishErase(LRUHandle* e) {
    if (e != nullptr) {
        assert(e->in_cache);
        // 将缓存项 e 从 in_use_ 或 lru_ 链表中移除。
        LRU_Remove(e);
        e->in_cache = false;
        usage_ -= e->charge;
        // 将引用计数减一，如果减一后为零，则销毁该缓存项。
        Unref(e);
    }
    return e != nullptr;
}

void LRUCache::Erase(const Slice& key, uint32_t hash) {
    MutexLock l(&mutex_);
    // 先从 Hash 表中移除 key 对应的缓存项，然后调用 FinishErase
    // 将缓存项从 Cache 中移除。
    FinishErase(table_.Remove(key, hash));
}

void LRUCache::Prune() {
    MutexLock l(&mutex_);
    // 遍历 lru_ 链表，将该链表上的所有缓存项从 Cache 中移除。
    while (lru_.next != &lru_) {
        LRUHandle* e = lru_.next;
        assert(e->refs == 1);
        bool erased = FinishErase(table_.Remove(e->key(), e->hash));
        if (!erased) {  // to avoid unused variable when compiled NDEBUG
            assert(erased);
        }
    }
}

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

class ShardedLRUCache : public Cache {
   private:
    LRUCache shard_[kNumShards]; // Cache 内拥有 kNumShards(16) 个 LRUCache
    port::Mutex id_mutex_;
    uint64_t last_id_; // 最后一个插入到 Cache 的缓存项的 ID

    static inline uint32_t HashSlice(const Slice& s) { return Hash(s.data(), s.size(), 0); }

    static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

   public:
    // capacity: ShardedLRUCache 的总容量
    // 根据总容量计算每个 shard 里的 LRUCache 的容量。
    explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
        // 计算 per_shard: 每个 shard 的容量。
        // per_shard = ⌈capacity / kNumShards⌉ (向上取整)
        const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
        for (int s = 0; s < kNumShards; s++) {
            // 给每个 shard 里的 LRUCache 设置容量。
            shard_[s].SetCapacity(per_shard);
        }
    }
    ~ShardedLRUCache() override {}
    Handle* Insert(const Slice& key, void* value, size_t charge,
                   void (*deleter)(const Slice& key, void* value)) override {
        // 计算 key 的 hash 值，然后根据 hash 值选择一个 shard，
        // 将 key 插入到该 shard 的 LRUCache 中。
        const uint32_t hash = HashSlice(key);
        return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
    }
    Handle* Lookup(const Slice& key) override {
        // 使用与 Insert 相同的 Hash 算法计算 key 的 hash 值，
        // 找到对应的 shard，然后在该 shard 的 LRUCache 中查找 key。
        const uint32_t hash = HashSlice(key);
        return shard_[Shard(hash)].Lookup(key, hash);
    }
    void Release(Handle* handle) override {
        // Handle 中已经存好 hash 值了，不需要重新计算。
        // 找到对应的 shard，然后让该 shard 的 LRUCache 释放 handle。
        LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
        shard_[Shard(h->hash)].Release(handle);
    }
    void Erase(const Slice& key) override {
        // 使用与 Insert 相同的 Hash 算法计算 key 的 hash 值，
        // 找到对应的 shard，然后让该 shard 的 LRUCache 移除 key。
        const uint32_t hash = HashSlice(key);
        shard_[Shard(hash)].Erase(key, hash);
    }
    void* Value(Handle* handle) override { return reinterpret_cast<LRUHandle*>(handle)->value; }
    uint64_t NewId() override {
        MutexLock l(&id_mutex_);
        return ++(last_id_);
    }
    void Prune() override {
        for (int s = 0; s < kNumShards; s++) {
            shard_[s].Prune();
        }
    }
    size_t TotalCharge() const override {
        size_t total = 0;
        for (int s = 0; s < kNumShards; s++) {
            total += shard_[s].TotalCharge();
        }
        return total;
    }
};

}  // end anonymous namespace

Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

}  // namespace leveldb
