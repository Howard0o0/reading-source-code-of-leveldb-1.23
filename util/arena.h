// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ARENA_H_
#define STORAGE_LEVELDB_UTIL_ARENA_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace leveldb {

class Arena {
   public:
    Arena();

    // 不允许拷贝
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    ~Arena();

    // Return a pointer to a newly allocated memory block of "bytes" bytes.
    // 等同于malloc(bytes)
    char* Allocate(size_t bytes);

    // Allocate memory with the normal alignment guarantees provided by malloc.
    // 内存对齐版的Allocate
    char* AllocateAligned(size_t bytes);

    // Returns an estimate of the total memory usage of data allocated
    // by the arena.
    // 返回至今为止分配的内存总量
    size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

   private:
    char* AllocateFallback(size_t bytes);
    char* AllocateNewBlock(size_t block_bytes);

    // Allocation state
    char* alloc_ptr_;
    size_t alloc_bytes_remaining_;

    // Array of new[] allocated memory blocks
    std::vector<char*> blocks_;

    // Total memory usage of the arena.
    //
    // TODO(costan): This member is accessed via atomics, but the others are
    //               accessed without any locking. Is this OK?
    std::atomic<size_t> memory_usage_;
};

inline char* Arena::Allocate(size_t bytes) {
    // The semantics of what to return are a bit messy if we allow
    // 0-byte allocations, so we disallow them here (we don't need
    // them for our internal use).
    // Allocate(0)没有意义
    assert(bytes > 0);

    // 如果申请的bytes小于当前block剩余的bytes，
    // 就从block的剩余bytes里分配。
    // 并更新alloc_ptr_和alloc_bytes_remaining_
    if (bytes <= alloc_bytes_remaining_) {
        // 当前block的alloc_ptr_就是需要返回的地址，
        char* result = alloc_ptr_;
        // 更新下一次分配的起始地址
        alloc_ptr_ += bytes;
        // 更新当前block剩余可分配的bytes
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    // 如果申请的bytes大于当前block剩余的bytes，
    // 使用AllocateFallback(bytes)分配
    return AllocateFallback(bytes);
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ARENA_H_
