// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/comparator.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>

#include "leveldb/slice.h"

#include "util/logging.h"
#include "util/no_destructor.h"

namespace leveldb {

Comparator::~Comparator() = default;

namespace {
class BytewiseComparatorImpl : public Comparator {
   public:
    BytewiseComparatorImpl() = default;

    const char* Name() const override { return "leveldb.BytewiseComparator"; }

    int Compare(const Slice& a, const Slice& b) const override { return a.compare(b); }

    void FindShortestSeparator(std::string* start, const Slice& limit) const override {
        // Find length of common prefix
        // 先计算出 start 和 limit 的最短长度
        size_t min_length = std::min(start->size(), limit.size());
        // diff_index 用于记录 start 和 limit 第一个不同的字符的位置
        size_t diff_index = 0;
        while ((diff_index < min_length) && ((*start)[diff_index] == limit[diff_index])) {
            diff_index++;
        }

        if (diff_index >= min_length) {
            // Do not shorten if one string is a prefix of the other
            // 走到这就表示 start 是 limit 的前缀。
            // 这种 case 下，start 本身就是最短的分隔符，可以直接返回。
        } else {
            // 获取 start 的 第 diff_index 个字符
            uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
            // 先判断 diff_byte < 0xff 的意义在于
            // 防止 diff_byte + 1 溢出，导致结果不正确。
            if (diff_byte < static_cast<uint8_t>(0xff) &&
                diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
                // start 与 limit 的前 diff_index 个字符都相同，
                // 只需要把 start 的第 diff_index 个字符加 1 ，
                // 就得到 Shortest Seperator 了。
                (*start)[diff_index]++;
                start->resize(diff_index + 1);
                assert(Compare(*start, limit) < 0);
            }
        }
    }

    void FindShortSuccessor(std::string* key) const override {
        // Find first character that can be incremented
        size_t n = key->size();
        for (size_t i = 0; i < n; i++) {
            const uint8_t byte = (*key)[i];
            // 找到第一个小于 0xff 的字符，然后加 1
            // 以该字符为上界。 
            if (byte != static_cast<uint8_t>(0xff)) {
                (*key)[i] = byte + 1;
                key->resize(i + 1);
                return;
            }
        }
        // *key is a run of 0xffs.  Leave it alone.
    }
};
}  // namespace

const Comparator* BytewiseComparator() {
    static NoDestructor<BytewiseComparatorImpl> singleton;
    return singleton.get();
}

}  // namespace leveldb
