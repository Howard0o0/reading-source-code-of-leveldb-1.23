// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_

#include <cstdint>
#include <vector>

#include "leveldb/slice.h"

namespace leveldb {

struct Options;

class BlockBuilder {
   public:
    explicit BlockBuilder(const Options* options);

    BlockBuilder(const BlockBuilder&) = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;

    // 恢复到一个空白 Block 的状态
    void Reset();

    // 往 block buffer 里添加一对 Key-Value
    void Add(const Slice& key, const Slice& value);

    // 将重启点信息也压入到 Block 缓冲区里，结束该 Block 的构建，
    // 然后返回 buffer_。
    Slice Finish();

    // 返回 Block 的预估大小。
    // 准确的说，是 Block 的原始大小，但是 Block 写入
    // SST 前会进行压缩，所以此处只能返回一个预估大小。
    size_t CurrentSizeEstimate() const;

    // 判断 buffer_ 是否为空
    bool empty() const { return buffer_.empty(); }

   private:
    const Options* options_;         /* Options 对象 */
    std::string buffer_;             /* User Space 缓冲区 */
    std::vector<uint32_t> restarts_; /* Restart Points 数组 */
    int counter_;                    /* Entry 计数器，用于重启点的计算 */
    bool finished_;                  /* 是否已经调用了 Finish() 方法 */
    std::string last_key_;           /* 最后添加的 Key */
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
