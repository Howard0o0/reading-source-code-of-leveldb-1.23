// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"

namespace leveldb {

static const int kBlockSize = 4096;

Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

Arena::~Arena() {
    for (size_t i = 0; i < blocks_.size(); i++) {
        delete[] blocks_[i];
    }
}

// AllocateFallback的核心思想：
//     - 大内存直接找os要
//     - 小内存从block中分配
// 以此减少内存碎片的同时，又能保证效率。 
// 因为：
//     - 申请大内存的几率比较小，不会很频繁，找os要虽然慢但是可以避免内存碎片。
//     - 申请小内存的几率大，会比较频繁，从block中分配，效率高并且碎片也少。
char* Arena::AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) {
        // Object is more than a quarter of our block size.  Allocate it
        // separately to avoid wasting too much space in leftover bytes.
        // 当分配的内存大于块的1/4时，直接找os要，不从block中分配。
        char* result = AllocateNewBlock(bytes);
        return result;
    }

    // We waste the remaining space in the current block.
    alloc_ptr_ = AllocateNewBlock(kBlockSize);
    alloc_bytes_remaining_ = kBlockSize;

    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
}

char* Arena::AllocateAligned(size_t bytes) {
    // 计算对齐的长度，最小对齐长度为8。
    // 如果当前平台的字长大于8，则对齐长度为字长。
    const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;

    // x & (x-1) 是个位运算的技巧，用于快速的将x的最低一位1置为0。
    // 比如x = 0b1011, 则x & (x-1) = 0b1010。
    // 此处用(align & (align - 1)) == 0)快速判断align是否为2的幂。
    // 因为2的幂的二进制表示总是只有一位为1，所以x & (x-1) == 0。
    static_assert((align & (align - 1)) == 0,
                  "Pointer size should be a power of 2");

    // 位运算技巧，等同于 current_mod = alloc_ptr_ % align
    size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);

    // 为了对齐，多分配slop个字节。
    size_t slop = (current_mod == 0 ? 0 : align - current_mod);
    size_t needed = bytes + slop;

    char* result;
    if (needed <= alloc_bytes_remaining_) {
        // 向后移动alloc_ptr_, 
        // 将alloc_ptr_对齐到align的整数倍。
        result = alloc_ptr_ + slop;
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
    } else {
        // AllocateFallback always returned aligned memory
        // AllocateFallback本身就是对齐的，所以直接调用即可。
        // 因为AllocateFallback要么从os直接分配,
        // 要么new一个4KB的block，返回block的首地址
        result = AllocateFallback(bytes);
    }
    assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
    return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
    // 找os拿一块block_bytes大小的内存，
    char* result = new char[block_bytes];

    // 将该block放到blocks_中，
    // Arena销毁的时候一起释放。
    blocks_.push_back(result);

    // 记录分配的内存量。
    memory_usage_.fetch_add(block_bytes + sizeof(char*),
                            std::memory_order_relaxed);
    return result;
}

}  // namespace leveldb
