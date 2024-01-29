// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_VERSION_EDIT_H_
#define STORAGE_LEVELDB_DB_VERSION_EDIT_H_

#include "db/dbformat.h"
#include <set>
#include <utility>
#include <vector>

namespace leveldb {

class VersionSet;

/* 记录了一个 SSTable 的元信息 */
struct FileMetaData {
    FileMetaData() : refs(0), allowed_seeks(1 << 30), file_size(0) {}

    int refs;             /* 引用计数，表示当前 SSTable 被多少个 Version 所引用 */
    int allowed_seeks;    // 该 SST 允许被无效查找的次数。
    uint64_t number;      /* SSTable 文件记录编号 */
    uint64_t file_size;   /* SSTable 文件大小 */
    InternalKey smallest; /* 最小 Key 值 */
    InternalKey largest;  /* 最大 Key 值 */
};

/* Version N + VersionEdit => Version N+1，VersionEdit 记录了增量 */
class VersionEdit {
   public:
    VersionEdit() { Clear(); }
    ~VersionEdit() = default;

    void Clear();

    // 设置 Comparactor Name，默认为 leveldb.BytewiseComparator 
    void SetComparatorName(const Slice& name) {
        has_comparator_ = true;
        comparator_ = name.ToString();
    }

    // 设置对应的 WAL 编号
    void SetLogNumber(uint64_t num) {
        has_log_number_ = true;
        log_number_ = num;
    }

    // 设置前一个 WAL 编号
    void SetPrevLogNumber(uint64_t num) {
        has_prev_log_number_ = true;
        prev_log_number_ = num;
    }

    // 设置下一个文件编号
    void SetNextFile(uint64_t num) {
        has_next_file_number_ = true;
        next_file_number_ = num;
    }

    // 设置最大的 SequenceNumber
    void SetLastSequence(SequenceNumber seq) {
        has_last_sequence_ = true;
        last_sequence_ = seq;
    }

    // 设置 level 层本次进行 Compaction 的最后一个 InternalKey
    // 也就是下一次进行 Compaction 的起始 InternalKey。
    void SetCompactPointer(int level, const InternalKey& key) {
        compact_pointers_.push_back(std::make_pair(level, key));
    }

    // Add the specified file at the specified number.
    // REQUIRES: This version has not been saved (see VersionSet::SaveTo)
    // REQUIRES: "smallest" and "largest" are smallest and largest keys in file
    // 
    // 将一个 SST(MetaData)添加到 VersionEdit 中
    void AddFile(int level, uint64_t file, uint64_t file_size, const InternalKey& smallest,
                 const InternalKey& largest) {
        // 创建一个 FileMetaData 对象，
        // 将 number, file_size, smallest, largest 赋值给 FileMetaData 对象
        FileMetaData f;
        f.number = file;
        f.file_size = file_size;
        f.smallest = smallest;
        f.largest = largest;

        // 将该 FileMetaData 对象添加到 VersionEdit::new_files_ 中
        new_files_.push_back(std::make_pair(level, f));
    }

    // Delete the specified "file" from the specified "level".
    //
    // 从 level 层中移除一个 SST
    void RemoveFile(int level, uint64_t file) {
        deleted_files_.insert(std::make_pair(level, file));
    }

    // VersionEdit 会被序列化成 string，然后写入到 MANIFEST 文件中。
    // 或者从 MANIFEST 文件中读取出来，反序列化成 VersionEdit。

    // 将 VersionEdit 序列化成 string
    void EncodeTo(std::string* dst) const;
    // 将 string 反序列化成 VersionEdit
    Status DecodeFrom(const Slice& src);

    std::string DebugString() const;

   private:
    friend class VersionSet;

    /* 其中的 pair 为 level + 文件编号，表示被删除的 .ldb 文件 */
    typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;

    std::string comparator_;       /* Comparator 名称 */
    uint64_t log_number_;          /* 日志编号 */
    uint64_t prev_log_number_;     /* 前一个日志编号 */
    uint64_t next_file_number_;    /* 下一个文件编号 */
    SequenceNumber last_sequence_; /* 最大序列号 */

    bool has_comparator_; /* 上面 5 个变量的 Exist 标志位*/
    bool has_log_number_;
    bool has_prev_log_number_;
    bool has_next_file_number_;
    bool has_last_sequence_;

    /* 记录某一层下一次进行 Compaction 的起始 InternalKey */
    std::vector<std::pair<int, InternalKey>> compact_pointers_;

    DeletedFileSet deleted_files_; /* 记录哪些文件被删除了 */

    /* 记录哪一层新增了哪些 .ldb 文件，并且使用 FileMetaData 来表示 */
    std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_EDIT_H_
