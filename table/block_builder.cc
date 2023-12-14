// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// BlockBuilder generates blocks where keys are prefix-compressed:
//
// When we store a key, we drop the prefix shared with the previous
// string.  This helps reduce the space requirement significantly.
// Furthermore, once every K keys, we do not apply the prefix
// compression and store the entire key.  We call this a "restart
// point".  The tail end of the block stores the offsets of all of the
// restart points, and can be used to do a binary search when looking
// for a particular key.  Values are stored as-is (without compression)
// immediately following the corresponding key.
//
// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.

#include "table/block_builder.h"

#include <algorithm>
#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/options.h"

#include "util/coding.h"

namespace leveldb {

BlockBuilder::BlockBuilder(const Options* options)
    : options_(options), restarts_(), counter_(0), finished_(false) {
    assert(options->block_restart_interval >= 1);
    restarts_.push_back(0);  // First restart point is at offset 0
}

void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);  // First restart point is at offset 0
    counter_ = 0;
    finished_ = false;
    last_key_.clear();
}

/* 返回 Block 未压缩之前的大小 */
size_t BlockBuilder::CurrentSizeEstimate() const {
    return (buffer_.size() +                       // Raw data buffer
            restarts_.size() * sizeof(uint32_t) +  // Restart array
            sizeof(uint32_t));                     // Restart array length
}

Slice BlockBuilder::Finish() {
    // 先把 restarts_ 中的所有重启点位置压入 buffer_ 中
    for (size_t i = 0; i < restarts_.size(); i++) {
        PutFixed32(&buffer_, restarts_[i]);
    }

    // 再将重启点的数量压入到 buffer_ 中
    PutFixed32(&buffer_, restarts_.size());

    // 设置结束标志位
    finished_ = true;
    
    // 返回完整的 buffer_ 内容
    return Slice(buffer_);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
    // 获取上一个添加的 Key: last_key_ 
    Slice last_key_piece(last_key_);
    assert(!finished_);
    assert(counter_ <= options_->block_restart_interval);
    // 如果 buffer_ 非空，检查 key 是否大于最后一个被添加到 Block 中的 Key
    assert(buffer_.empty()  // No values yet?
           || options_->comparator->Compare(key, last_key_piece) > 0);

    // 初始化一个变量shared，
    // 用于记录新添加的键和上一次添加的键的共享前缀的长度。
    size_t shared = 0;

    // 如果 counter_ < block_restart_interval的话，
    // 说明还没有到重启点，计算当前键和上一次添加的键的共享前缀的长度。
    if (counter_ < options_->block_restart_interval) {
        // 计算当前键和上一次添加的键的共享前缀的长度 shared
        const size_t min_length = std::min(last_key_piece.size(), key.size());
        while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
            shared++;
        }
    } else {
        // counter_ == block_restart_interval，
        // 满足新建重启点的条件了，重置 counter_ 为 0，
        // 并且将当前的偏移量压入 restarts_ 数组中，记录当前重启点的位置。
        // 重启点的第一个 Key 不需要进行前缀压缩，
        // 所以此时保持 shared 的值为 0 即可。
        restarts_.push_back(buffer_.size());
        counter_ = 0;
    }

    // 计算 key 和 last_key_ 的非共享长度
    const size_t non_shared = key.size() - shared;

    // 使用变长编码，将 "<shared><non_shared><value_size>" 写入 buffer_
    PutVarint32(&buffer_, shared);
    PutVarint32(&buffer_, non_shared);
    PutVarint32(&buffer_, value.size());

    // 将 Key 非共享内容压入 buffer_ 
    buffer_.append(key.data() + shared, non_shared);
    // 将完整的 Value 压入 buffer_
    buffer_.append(value.data(), value.size());

    // 更新 last_key_
    last_key_.resize(shared);
    last_key_.append(key.data() + shared, non_shared);
    assert(Slice(last_key_) == key);
    counter_++;
}

}  // namespace leveldb
