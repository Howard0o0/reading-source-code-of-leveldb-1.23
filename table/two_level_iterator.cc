// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"

#include "leveldb/table.h"

#include "table/block.h"
#include "table/format.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {

typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);

class TwoLevelIterator : public Iterator {
   public:
    TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                     const ReadOptions& options);

    ~TwoLevelIterator() override;

    void Seek(const Slice& target) override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Next() override;
    void Prev() override;

    bool Valid() const override { return data_iter_.Valid(); }
    Slice key() const override {
        assert(Valid());
        // 取出 data_iter_ 当前位置的 key。
        return data_iter_.key();
    }
    Slice value() const override {
        assert(Valid());
        // 取出 data_iter_ 当前位置的 value。
        return data_iter_.value();
    }
    Status status() const override {
        // It'd be nice if status() returned a const Status& instead of a Status
        if (!index_iter_.status().ok()) {
            // 先检查 index_iter_ 是否有异常
            return index_iter_.status();
        } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
            // 再看 data_iter_ 是否有异常
            return data_iter_.status();
        } else {
            // index_iter_ 和 data_iter_ 都没异常，
            // 返回 TwoLevelIterator 自己的状态信息。
            return status_;
        }
    }

   private:
    void SaveError(const Status& s) {
        if (status_.ok() && !s.ok()) status_ = s;
    }
    void SkipEmptyDataBlocksForward();
    void SkipEmptyDataBlocksBackward();
    void SetDataIterator(Iterator* data_iter);
    void InitDataBlock();

    BlockFunction block_function_;
    void* arg_;
    const ReadOptions options_;
    Status status_;
    IteratorWrapper index_iter_;
    IteratorWrapper data_iter_;  // May be nullptr
    // If data_iter_ is non-null, then "data_block_handle_" holds the
    // "index_value" passed to block_function_ to create the data_iter_.
    std::string data_block_handle_;
};

TwoLevelIterator::TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                                   const ReadOptions& options)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(nullptr) {}

TwoLevelIterator::~TwoLevelIterator() = default;

void TwoLevelIterator::Seek(const Slice& target) {
    // index_iter_ 对应着 index_block，到 index_block 中
    // 查找 target 属于哪个 data_block。
    index_iter_.Seek(target);
    // 如果找到了这个 data_block，就从 SST 中加载这个 data_block。
    InitDataBlock();
    // data_iter_.iter() != nullptr 从 SST 中加载到 target
    // 所属的 data_block了。
    if (data_iter_.iter() != nullptr) data_iter_.Seek(target);
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToFirst() {
    // 到 index_block 中查找第一个 data_block 是哪个。
    index_iter_.SeekToFirst();
    // 从 SST 中加载这个 data_block。
    InitDataBlock();
    // data_iter_.iter() != nullptr 从 SST 中加载到第一个有效的
    // data_block 了。
    if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() {
    // 到 index_block 中查找最后一个 data_block 是哪个。
    index_iter_.SeekToLast();
    // 从 SST 中加载这个 data_block。
    InitDataBlock();
    // data_iter_.iter() != nullptr 从 SST 中加载到第一个有效的
    // data_block 了。
    if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() {
    assert(Valid());
    // 将 data_iter_ 往后移一位即可。
    data_iter_.Next();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() {
    assert(Valid());
    // 将 data_iter_ 往前移一位即可。
    data_iter_.Prev();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::SkipEmptyDataBlocksForward() {
    // 如果 index_iter_ 对应的 data_iter_ 里没有有效数据，
    // 就将 index_iter_ 往后移一位, 加载下一个 data_block，
    // 一直到找到有数据的 data_block 为止。
    while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
        // Move to next block
        //
        // index_iter_ 无效，表示 index_block 已经遍历完了。
        // 此时将 data_iter_ 置为 null 表示 data_iter_ 已经
        // 到末尾了。
        if (!index_iter_.Valid()) {
            SetDataIterator(nullptr);
            return;
        }
        // index_iter_ 里还有东西，继续往后走一位。
        index_iter_.Next();
        // 加载与当前 index_iter_ 对应的 Data Block。
        InitDataBlock();
        // 如果 data_iter_ 有效，就将 data_iter_ 移到第一个有效的位置。
        if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
    }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
    while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
        // Move to next block
        if (!index_iter_.Valid()) {
            SetDataIterator(nullptr);
            return;
        }
        index_iter_.Prev();
        InitDataBlock();
        if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
    }
}

void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
    if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
    data_iter_.Set(data_iter);
}

void TwoLevelIterator::InitDataBlock() {
    if (!index_iter_.Valid()) {
        // 如果 index_iter_ 无效，那对应的 data_iter_ 也是无效的。
        SetDataIterator(nullptr);
    } else {
        // 从 index_iter 中取出对应的 data_block 的 handle。
        Slice handle = index_iter_.value();
        if (data_iter_.iter() != nullptr && handle.compare(data_block_handle_) == 0) {
            // data_iter_ is already constructed with this iterator, so
            // no need to change anything
            //
            // 与 handle 对应的 data_iter_ 已经构建好了，
            // 不需要重复构建。
        } else {
            // 通过 block_function_ 构建 与 handle 对应的 data_iter_。
            Iterator* iter = (*block_function_)(arg_, options_, handle);
            data_block_handle_.assign(handle.data(), handle.size());
            SetDataIterator(iter);
        }
    }
}

}  // namespace

Iterator* NewTwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                              const ReadOptions& options) {
    return new TwoLevelIterator(index_iter, block_function, arg, options);
}

}  // namespace leveldb
