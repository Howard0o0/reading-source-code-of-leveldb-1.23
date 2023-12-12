// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
#define STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_

#include <string>

#include "leveldb/export.h"

namespace leveldb {

class Slice;

// A Comparator object provides a total order across slices that are
// used as keys in an sstable or a database.  A Comparator implementation
// must be thread-safe since leveldb may invoke its methods concurrently
// from multiple threads.
class LEVELDB_EXPORT Comparator {
   public:
    virtual ~Comparator();

    // Three-way comparison.  Returns value:
    //   < 0 iff "a" < "b",
    //   == 0 iff "a" == "b",
    //   > 0 iff "a" > "b"
    // 用于比较 Key a 和 Key b。
    //   - 如果 a < b，返回值 < 0；
    //   - 如果 a == b，返回值 == 0；
    //   - 如果 a > b，返回值 > 0。
    virtual int Compare(const Slice& a, const Slice& b) const = 0;

    // The name of the comparator.  Used to check for comparator
    // mismatches (i.e., a DB created with one comparator is
    // accessed using a different comparator.
    //
    // The client of this package should switch to a new name whenever
    // the comparator implementation changes in a way that will cause
    // the relative ordering of any two keys to change.
    //
    // Names starting with "leveldb." are reserved and should not be used
    // by any clients of this package.
    // Comparator 的名称，用于检查 Comparator 是否匹配。
    // 比如数据库创建时使用的是 Comparator A，
    // 重新打开数据库时使用的是 Comparator B，
    // 此时 LevelDB 则会检测到 Comparator 不匹配。
    virtual const char* Name() const = 0;

    // Advanced functions: these are used to reduce the space requirements
    // for internal data structures like index blocks.

    // If *start < limit, changes *start to a short string in [start,limit).
    // Simple comparator implementations may return with *start unchanged,
    // i.e., an implementation of this method that does nothing is correct.
    // 找到一个最短的字符串 seperator，使得 start <= seperator < limit，
    // 并将结果保存在 start 中。
    // 用于 SST 中 Data Block 的索引构建。
    virtual void FindShortestSeparator(std::string* start, const Slice& limit) const = 0;

    // Changes *key to a short string >= *key.
    // Simple comparator implementations may return with *key unchanged,
    // i.e., an implementation of this method that does nothing is correct.
    // 找到一个最短的字符串 successor，使得 key <= successor，
    // 并将结果保存在 key 中。
    // 用于 SST 中最后一个 Data Block 的索引构建。
    virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Return a builtin comparator that uses lexicographic byte-wise
// ordering.  The result remains the property of this module and
// must not be deleted.
LEVELDB_EXPORT const Comparator* BytewiseComparator();

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
