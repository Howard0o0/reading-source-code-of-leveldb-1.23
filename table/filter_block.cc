// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"

#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy) : policy_(policy) {}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
    // 每个 Filter 的大小是固定的，kFilterBase，默认为 2KB。
    // Filter 与 Data Block 是分组对应的关系。
    // filter_index 用于记录当前 Data Block 对应的 Filter 的索引。
    // 比如，Data Block 的大小是 4KB，那么：
    //   - Data Block 0(block_offset = 0) 对应 Filter 0
    //   - Data Block 1(block_offset = 4KB) 对应 Filter 2
    //   - Data Block 2(block_offset = 8KB) 对应 Filter 4
    // Filter 1, 3, 5 是空的，不对应任何 Data Block。
    uint64_t filter_index = (block_offset / kFilterBase);

    assert(filter_index >= filter_offsets_.size());

    // filter_offsets_ 中记录了每个 Filter 的起始偏移量。
    // 换句话说，filter_offsets_.size() 就是已经构造好的 Filter 数量。
    // 在记录新的 Key 之前，需要先把 buffer 里积攒的 Key (属于上一个 Data Block 的 Key)都构造成 Filter。
    // 这里的 while 是上一个 Data Block 构建 Filter，然后清空 filter buffer，为新的 Data Block 做准备。
    // 这里的 while 理解起来会有点误解，看上去好像可能会为了 1 个 Data Block 构建多个 Filter 的样子，其实不是。
    // 假设 Data Block 大小为 4KB, Filter 大小为 2KB，那么 Filter 0 对应 Data Block 0，Filter 1 是个空壳，不对应任何 Data Block。
    // 假设 Data Block 的大小为 1KB，Filter 的大小为 2KB，那么就会有 1 个 Filter 对应 2 个 Data Block。
    while (filter_index > filter_offsets_.size()) {
        GenerateFilter();
    }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
    Slice k = key;
    // std::vector<size_t> start_ 里存储的是每个 key 在 keys_ 中的位置。
    start_.push_back(keys_.size());
    // std::string keys_ 里存放的是所有的 key，是一个所有 key 拼接起来的字符串。
    keys_.append(k.data(), k.size());
}

Slice FilterBlockBuilder::Finish() {
    if (!start_.empty()) {
        GenerateFilter();
    }

    const uint32_t array_offset = result_.size();
    /* 将所有的偏移量放到 result_ 尾部，偏移量为定长编码 */
    for (size_t i = 0; i < filter_offsets_.size(); i++) {
        PutFixed32(&result_, filter_offsets_[i]);
    }

    /* 将 Bloom Filters 的个数扔到 result_ 尾部*/
    PutFixed32(&result_, array_offset);
    /* 将 "base" 的大小放入，因为 kFilterBaseLg 可能会被修改 */
    result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
    return Slice(result_);
}

// 生成一个 filter，然后压入到 result_ 中
void FilterBlockBuilder::GenerateFilter() {
    // std::vector<size_t> start_ 里存储的是每个 key 在 std::string keys_ 中的位置。
    // start_.size() 代表的是 keys_ 缓冲区中的 key 的数量。
    const size_t num_keys = start_.size();

    // std::string result_ 里存放的是每个 filter 拍平后的数据，也就是说，
    // filter-0, filter-1, ..., filter-n 是按顺序连续存放到 result_ 里的。
    // 
    // std::vector<uint32_t> filter_offsets_ 里则存放每个 filter 在 result_ 里的位置，
    // 比如 filter_offsets[0] 代表的是 filter-0 在 result_ 里的位置。
    // 
    // 此处如果 num_keys 为 0，则构造一个空的 dummy filter，但是将该 filter 的
    // 位置 filter_offsets_[i] 记录为上一个 filter 的位置。
    // 比如 filter_offsets[1] = filter_offsets[0]，filter-0 是有实际数据的，
    // 但 filter-1 是个空的 dummy filter，就把 filter_offsets[1] 指向 filter-0。
    // 这主要用于 Data Block 的大于 Filter 大小的情况，参考 FilterBlockBuilder::StartBlock
    // 中的注释可更好理解。
    if (num_keys == 0) {
        // Fast path if there are no keys for this filter
        // 只是往 filter_offsets_ 里压入一个位置，但是没有往 result_ 里压入 filter。
        filter_offsets_.push_back(result_.size());
        return;
    }

    // 此处往 start_ 里压入下一个 key 起始位置是为了方便计算 keys_ 中最后一个 key 的长度。
    // keys_[i] 的长度计算方式为 start_[i+1] - start_[i]。
    // 假设 keys_ 里一共有 n 个 key，那么 start_ 里一共有 n 个元素，如果我们要计算
    // 最后一个 key 的长度，则为 start_[n] - start[n-1]，但是 start_[n] 并不存在，越界了。
    // 所以这里先压入一个 start_[n]，就可以计算出最后一个 key 的长度了。
    // 反正 GenerateFilter() 结束后 start_ 就会被清空重置了，所以这里修改 start_ 没有问题。
    start_.push_back(keys_.size());  

    /* tmp_keys_ 主要的作用就是作为 CreateFilter() 方法参数构建 Bloom Filter */
    // tmp_keys_ 只是一个临时的富足
    tmp_keys_.resize(num_keys);

    // 从 keys_ 中取出所有的 key，放到 std::vector<Slice> tmp_keys_ 中。
    // 不太懂为什么 leveldb 不直接把 keys_ 设为 std::vector<Slice> 类型 = =。
    for (size_t i = 0; i < num_keys; i++) {
        // 取 keys_[i] 的起始地址
        const char* base = keys_.data() + start_[i]; 
        // 计算 keys[i] 的长度，
        // 现在可以看到上面 start_.push_back(keys_.size()) 的作用了
        size_t length = start_[i + 1] - start_[i];   
        // 把 keys_[i] 放到 tmp_keys_[i] 中
        tmp_keys_[i] = Slice(base, length);          
    }

    // 先把新 filter 的位置记录到 filter_offsets_ 中
    filter_offsets_.push_back(result_.size());
    // 再构造新 filter，然后压入到 result_ 中
    policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

    // 新 filter 构造完毕，把缓冲区清空，为下一个 filter 做准备。
    tmp_keys_.clear();
    keys_.clear();
    start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy, const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
    size_t n = contents.size();
    if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
    base_lg_ = contents[n - 1];
    uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
    if (last_word > n - 5) return;
    data_ = contents.data();
    offset_ = data_ + last_word;
    num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
    uint64_t index = block_offset >> base_lg_;
    if (index < num_) {
        uint32_t start = DecodeFixed32(offset_ + index * 4);
        uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
        if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
            Slice filter = Slice(data_ + start, limit - start);
            return policy_->KeyMayMatch(key, filter);
        } else if (start == limit) {
            // Empty filters do not match any keys
            return false;
        }
    }
    return true;  // Errors are treated as potential matches
}

}  // namespace leveldb
