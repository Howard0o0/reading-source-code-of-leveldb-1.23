// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// TableBuilder provides the interface used to build a Table
// (an immutable and sorted map from keys to values).
//
// Multiple threads can invoke const methods on a TableBuilder without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same TableBuilder must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
#define STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/options.h"
#include "leveldb/status.h"

namespace leveldb {

class BlockBuilder;
class BlockHandle;
class WritableFile;

/* TableBuilder 实现 */
class LEVELDB_EXPORT TableBuilder {
   public:
    // Create a builder that will store the contents of the table it is
    // building in *file.  Does not close the file.  It is up to the
    // caller to close the file after calling Finish().
    /* WritableFile 通常为 PosixWritableFile */
    TableBuilder(const Options& options, WritableFile* file);

    TableBuilder(const TableBuilder&) = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    // REQUIRES: Either Finish() or Abandon() has been called.
    ~TableBuilder();

    // Change the options used by this builder.  Note: only some of the
    // option fields can be changed after construction.  If a field is
    // not allowed to change dynamically and its value in the structure
    // passed to the constructor is different from its value in the
    // structure passed to this method, this method will return an error
    // without changing any fields.
    Status ChangeOptions(const Options& options);

    // Add key,value to the table being constructed.
    // REQUIRES: key is after any previously added key according to comparator.
    // REQUIRES: Finish(), Abandon() have not been called
    /* 向 TableBuilder 中添加 Key-Value */
    void Add(const Slice& key, const Slice& value);

    // Advanced operation: flush any buffered key/value pairs to file.
    // Can be used to ensure that two adjacent entries never live in
    // the same data block.  Most clients should not need to use this method.
    // REQUIRES: Finish(), Abandon() have not been called
    /* 结束当前 Block 的构建 */
    // Flush()方法用于将缓冲区中的键值对立即写入到文件中。
    // 这个方法主要用于确保两个相邻的条目永远不会在同一个数据块中。
    // 大多数情况不需要使用这个方法。调用Flush()方法后，TableBuilder仍然可以继续添加新的键值对。
    void Flush();

    // Return non-ok iff some error has been detected.
    Status status() const;

    // Finish building the table.  Stops using the file passed to the
    // constructor after this function returns.
    // REQUIRES: Finish(), Abandon() have not been called
    /* 结束 Table 的构建 */
    // Finish()方法用于完成SSTable的构建。
    // 调用这个方法后，TableBuilder将停止使用传递给构造函数的文件句柄，并将所有缓冲区中的键值对以及元数据写入到文件中。
    // 一旦调用了Finish()方法，就不能再向TableBuilder中添加新的键值对，也不能再次调用Finish()方法。
    Status Finish();

    // Indicate that the contents of this builder should be abandoned.  Stops
    // using the file passed to the constructor after this function returns.
    // If the caller is not going to call Finish(), it must call Abandon()
    // before destroying this builder.
    // REQUIRES: Finish(), Abandon() have not been called
    /* 放弃 Table 的构建 */
    // 放弃SSTable的构建。在调用此方法后，之前添加到TableBuilder中的所有键值对都将被丢弃。
    void Abandon();

    // Number of calls to Add() so far.
    /* 一共添加了多少 Key-Value 对 */
    uint64_t NumEntries() const;

    // Size of the file generated so far.  If invoked after a successful
    // Finish() call, returns the size of the final generated file.
    uint64_t FileSize() const;

   private:
    bool ok() const { return status().ok(); }
    /* 序列化需要写入的 Data Block */
    void WriteBlock(BlockBuilder* block, BlockHandle* handle);
    /* 将压缩后的数据写入文件中 */
    void WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

    struct Rep;
    Rep* rep_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
