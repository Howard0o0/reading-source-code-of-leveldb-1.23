// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The representation of a DBImpl consists of a set of Versions.  The
// newest version is called "current".  Older versions may be kept
// around to provide a consistent view to live iterators.
//
// Each Version keeps track of a set of Table files per level.  The
// entire set of versions is maintained in a VersionSet.
//
// Version,VersionSet are thread-compatible, but require external
// synchronization on all accesses.

#ifndef STORAGE_LEVELDB_DB_VERSION_SET_H_
#define STORAGE_LEVELDB_DB_VERSION_SET_H_

#include "db/dbformat.h"
#include "db/version_edit.h"
#include <map>
#include <set>
#include <vector>

#include "port/port.h"
#include "port/thread_annotations.h"

namespace leveldb {

namespace log {
class Writer;
}

class Compaction;
class Iterator;
class MemTable;
class TableBuilder;
class TableCache;
class Version;
class VersionSet;
class WritableFile;

// Return the smallest index i such that files[i]->largest >= key.
// Return files.size() if there is no such file.
// REQUIRES: "files" contains a sorted list of non-overlapping files.
int FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files,
             const Slice& key);

// Returns true iff some file in "files" overlaps the user key range
// [*smallest,*largest].
// smallest==nullptr represents a key smaller than all keys in the DB.
// largest==nullptr represents a key largest than all keys in the DB.
// REQUIRES: If disjoint_sorted_files, files[] contains disjoint ranges
//           in sorted order.
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp, bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files, const Slice* smallest_user_key,
                           const Slice* largest_user_key);

/* SSTable 版本控制。在 leveldb 中，一个 Version
 * 就表示了一个数据库版本，它记录了当前磁盘和内存中 的所有数据信息。CURRENT
 * 指向最新的 Version，所有的 Version 加起来就组成了 VersionSet。
 *
 * VersionEdit 表示一个增量（delta），那么 version 1 + VersionEdit = version 2
 */
class Version {
   public:
    // Lookup the value for key.  If found, store it in *val and
    // return OK.  Else return a non-OK status.  Fills *stats.
    // REQUIRES: lock is not held
    // GetStats 用于记录 Get 操作的一些额外返回信息:
    //   - 第一个被无效查找的 SST
    //   - 该无效查找 SST 所在的 level
    struct GetStats {
        FileMetaData* seek_file;
        int seek_file_level;
    };

    // Append to *iters a sequence of iterators that will
    // yield the contents of this Version when merged together.
    // REQUIRES: This version has been saved (see VersionSet::SaveTo)
    // 创建一个 iters，可以用来遍历当前版本的所有 key-value。
    void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

    // 在指定版本中，查找一个给定 key 的 value。
    // 如果该 key 存在，则返回 OK，并且在 stats 中记录一些查找信息，比如
    // key 所在的文件、level 等。
    Status Get(const ReadOptions&, const LookupKey& key, std::string* val, GetStats* stats);

    // Adds "stats" into the current state.  Returns true if a new
    // compaction may need to be triggered, false otherwise.
    // REQUIRES: lock is held
    // 该方法通常在 Get 操作后被调用，stats 中包含了 Get 操作所访问的 
    // SST 文件的 MetaData，MetaData 中有个成员 allowed_seeks, 表示该 SST
    // 允许被访问的次数。每调用一次 UpdateStats(stats)，会将 allowed_seeks 减一，
    // 当 allowed_seeks 为 0 时，会返回 true，
    // 表示该 SST 的访问频率过高，需要进行 Compaction。
    bool UpdateStats(const GetStats& stats);

    // Record a sample of bytes read at the specified internal key.
    // Samples are taken approximately once every config::kReadBytesPeriod
    // bytes.  Returns true if a new compaction may need to be triggered.
    // REQUIRES: lock is held
    // 通过 Iterator 每读取 config::kReadBytesPeriod 个字节，
    // 就会对当前 key 调用一次 RecordReadSample()，
    // 记录 key 的访问频率。
    // RecordReadSample(key) 会遍历所有与 key 有 overlap 的 SST，
    // 所以重叠的 SST 不止一个，就对第一个 SST 调用一次 UpdateStats。
    bool RecordReadSample(Slice key);

    // Reference count management (so Versions do not disappear out from
    // under live iterators)
    void Ref();
    void Unref();

    // 给定一个范围 [begin, end]，返回在 level 层所有与之有 overlap 的 SST 文件，
    // 记录到 inputs 中。
    void GetOverlappingInputs(int level,
                              const InternalKey* begin,  // nullptr means before all keys
                              const InternalKey* end,    // nullptr means after all keys
                              std::vector<FileMetaData*>* inputs);

    // Returns true iff some file in the specified level overlaps
    // some part of [*smallest_user_key,*largest_user_key].
    // smallest_user_key==nullptr represents a key smaller than all the DB's
    // keys. largest_user_key==nullptr represents a key largest than all the
    // DB's keys.
    // 检查 level 层是否有 SST 与 [smallest_user_key, largest_user_key] 有 overlap
    bool OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key);

    // Return the level at which we should place a new memtable compaction
    // result that covers the range [smallest_user_key,largest_user_key].
    // 给定 [smallest_user_key, largest_user_key] 代表一个 MemTable， 
    // 返回一个 level，用于将该 MemTable 落盘为 SST。
    int PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key);

    // 获取指定 level 上的 SST 数量。
    int NumFiles(int level) const { return files_[level].size(); }

    // Return a human readable string that describes this version's contents.
    // 用可视化的方式打印 Version 的内容。
    std::string DebugString() const;

   private:
    friend class Compaction;
    friend class VersionSet;

    class LevelFileNumIterator;

    explicit Version(VersionSet* vset)
        : vset_(vset),
          next_(this),
          prev_(this),
          refs_(0),
          file_to_compact_(nullptr),
          file_to_compact_level_(-1),
          compaction_score_(-1),
          compaction_level_(-1) {}

    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    ~Version();

    Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

    // Call func(arg, level, f) for every file that overlaps user_key in
    // order from newest to oldest.  If an invocation of func returns
    // false, makes no more calls.
    //
    // REQUIRES: user portion of internal_key == user_key.
    void ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                            bool (*func)(void*, int, FileMetaData*));

    VersionSet* vset_;  // VersionSet to which this Version belongs

    /* 多个 Version 之间组成双向链表 */
    Version* next_;  // Next version in linked list
    Version* prev_;  // Previous version in linked list
    int refs_;       // Number of live refs to this version

    /* 每一个 level 所包含的全部 .ldb 文件，由 FileMetaData 表示 */
    std::vector<FileMetaData*> files_[config::kNumLevels];

    /* Next file to compact based on seek stats.
     * 根据 Seek 的过程决定下一次选定的 Compaction 文件和目标 level */
    FileMetaData* file_to_compact_;
    int file_to_compact_level_;

    /*
     * Level that should be compacted next and its compaction score.
     * Score < 1 means compaction is not strictly needed.  These fields
     * are initialized by Finalize().
     *
     * leveldb 中的 Size Compaction 发生在某一个 level
     * 中文件总大小超过阈值时，score 是通过 `level 总文件大小 / 此 level 阈值`
     * 所得到的，那么 compaction_score_ 就是这些 score 中
     * 最大的那一个，也就是找出最需要 Compaction 的 level。
     * */
    double compaction_score_;
    int compaction_level_;
};

class VersionSet {
   public:
    VersionSet(const std::string& dbname, const Options* options, TableCache* table_cache,
               const InternalKeyComparator*);
    VersionSet(const VersionSet&) = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    ~VersionSet();

    // Apply *edit to the current version to form a new descriptor that
    // is both saved to persistent state and installed as the new
    // current version.  Will release *mu while actually writing to the file.
    // REQUIRES: *mu is held on entry.
    // REQUIRES: no other thread concurrently calls LogAndApply()
    //
    // 将 VersionEdit 应用到当前 Version，生成新的 Version，
    // 并将新 Version 加入到 VersionSet 中。
    // Version N + VersionEdit = Version N+1。
    Status LogAndApply(VersionEdit* edit, port::Mutex* mu) EXCLUSIVE_LOCKS_REQUIRED(mu);

    // Recover the last saved descriptor from persistent storage.
    // 从manifest文件中恢复数据库的状态。
    // 在LevelDB启动时，会调用这个方法来加载数据库的当前状态。
    Status Recover(bool* save_manifest);

    // Return the current version.
    // 返回当前的 Version。
    Version* current() const { return current_; }

    // Return the current manifest file number
    // 返回当前正在使用的 MANIFEST 文件编号。
    uint64_t ManifestFileNumber() const { return manifest_file_number_; }

    // Allocate and return a new file number
    // 生成一个新的文件编号。
    uint64_t NewFileNumber() { return next_file_number_++; }

    // Arrange to reuse "file_number" unless a newer file number has
    // already been allocated.
    // REQUIRES: "file_number" was returned by a call to NewFileNumber().
    // 
    // VersionSet::ReuseFileNumber方法用于在某些情况下重复使用SST文件编号。
    // 这通常在创建新的SST文件但未实际使用它，然后决定删除它的情况下发生。
    // 例如，当LevelDB进行压缩操作时，它会创建新的SST文件来存储压缩后的数据。
    // 然而，如果在创建新文件后，压缩操作由于某种原因（如错误或异常）被中断，
    // 那么新创建的SST文件可能就不再需要了。在这种情况下，LevelDB可以通过
    // 调用VersionSet::ReuseFileNumber方法来重复使用该SST文件的编号，
    // 而不是浪费一个新的编号。
    void ReuseFileNumber(uint64_t file_number) {
        // 假设上一次 NewFileNumer() 分配的编号为 100，同时
        // next_file_number_ 会更新为 101。
        // 上次分出去的编号 100 用不到了，使用 ReuseFileNumber(100)
        // 进行回收，就把 next_file_number_ 回退为 100 就行。
        // 这样下次 NewFileNumer() 又会把 100 分出去。
        if (next_file_number_ == file_number + 1) {
            next_file_number_ = file_number;
        }
    }

    // Return the number of Table files at the specified level.
    //
    // 返回某一层上的 SST 文件数量。
    int NumLevelFiles(int level) const;

    // Return the combined file size of all files at the specified level.
    // 
    // 返回某一层上 SST 文件的总大小。
    int64_t NumLevelBytes(int level) const;

    // Return the last sequence number.
    //
    // 返回当前数据库中最大的 Sequence Number。
    uint64_t LastSequence() const { return last_sequence_; }

    // Set the last sequence number to s.
    // 
    // 设置当前数据库中最大的 Sequence Number。
    void SetLastSequence(uint64_t s) {
        assert(s >= last_sequence_);
        last_sequence_ = s;
    }

    // Mark the specified file number as used.
    //
    // 标记某个文件编号已经被使用。
    void MarkFileNumberUsed(uint64_t number);

    // Return the current log file number.
    //
    // 返回当前正在使用的 WAL 文件编号。
    uint64_t LogNumber() const { return log_number_; }

    // Return the log file number for the log file that is currently
    // being compacted, or zero if there is no such log file.
    //
    // prev_log_number_ 记录了当前正在进行 Compaction 的 WAL 文件编号。
    // 当开始 Compaction 时，当前的 WAL 可能仍有一些正在写入的数据。为了
    // 确保这些数据不会丢失，LevelDB 并不会立即删除 WAL 文件。相反，它开始写入
    // 一个新的 WAL 文件，并在prev_log_number_中保留对旧日志文件（正在被压缩的文件）的引用。
    // 这样，即使在 Compaction 过程中发生崩溃，LevelDB 仍然可以从旧的日志文件中恢复数据。
    // 一旦 Compaction 完成并且旧 WAL 文件中的所有数据都安全地写入到SST文件中，
    // 旧的 WAL 文件就会被删除，prev_log_number_被设置为零。
    uint64_t PrevLogNumber() const { return prev_log_number_; }

    // Pick level and inputs for a new compaction.
    // Returns nullptr if there is no compaction to be done.
    // Otherwise returns a pointer to a heap-allocated object that
    // describes the compaction.  Caller should delete the result.
    // 
    // 选择一个合适的 Level 和 SST 文件集合进行 Compaction，
    // 用 Compaction 对象表示这次 Compaction 所需的信息。
    Compaction* PickCompaction();

    // Return a compaction object for compacting the range [begin,end] in
    // the specified level.  Returns nullptr if there is nothing in that
    // level that overlaps the specified range.  Caller should delete
    // the result.
    //
    // 指定 Level 和一个范围 [begin, end]，返回一个 Compaction 对象。
    Compaction* CompactRange(int level, const InternalKey* begin, const InternalKey* end);

    // Return the maximum overlapping data (in bytes) at next level for any
    // file at a level >= 1.
    //
    // 对每一层计算 level-i 与 level-i+1 之间的 overlap bytes。
    // 返回最大的 overlap bytes。
    int64_t MaxNextLevelOverlappingBytes();

    // Create an iterator that reads over the compaction inputs for "*c".
    // The caller should delete the iterator when no longer needed.
    //
    // 读取 Compaction 里包含的输入文件，创建一个可以遍历这些文件的 Iterator。
    Iterator* MakeInputIterator(Compaction* c);

    // Returns true iff some level needs a compaction.
    //
    // 判断当前是否需要进行 Compaction。
    bool NeedsCompaction() const {
        Version* v = current_;
        return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);
    }

    // Add all files listed in any live version to *live.
    // May also mutate some internal state.
    //
    // 将所有活跃的 SST 文件编号添加到 live 中。
    // 活跃的 SST 指正在参与 compaction 或者未过期的 SST。
    // 有些 SST 由于创建了快照，compaction 时没有将其删除。
    // 快照释放后，这些 SST 就过期了，不属于任何一个 level，但是仍然存在于磁盘上。
    void AddLiveFiles(std::set<uint64_t>* live);

    // Return the approximate offset in the database of the data for
    // "key" as of version "v".
    // 计算 key 在指定版本中的大致偏移量。
    // 假设该版本的数据库状态如下:
    //           +----------------------------+
    // Level-0   |  SST0      SST1      SST2  |
    //           +----------------------------+
    // Level-1   |  SST3      SST4      SST5  |
    //           +----------------------------+
    // Level-2   |            ...             |
    //           +----------------------------+
    // Level-3   |            ...             |
    //           +----------------------------+
    // Level-4   |            ...             |
    //           +----------------------------+
    //
    // 假设目标 key 是 SST4 中的第一个，每个 SST 的大小为 4KB，
    // 则`ApproximateOffsetOf`返回的 offset 为 4 * 4KB = 16KB。
    uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);

    // Return a human-readable short (single-line) summary of the number
    // of files per level.  Uses *scratch as backing store.
    struct LevelSummaryStorage {
        char buffer[100];
    };
    // 以可视化的方式打印每个 level 的 SST 数量。
    // 比如 scratch->buffer = "files[ 1, 5, 7, 9, 0, 0, 0]"
    // 表示 Level-0 有 1 个 SST，Level-1 有 5 个 SST，以此类推。
    const char* LevelSummary(LevelSummaryStorage* scratch) const;

   private:
    class Builder;

    friend class Compaction;
    friend class Version;

    bool ReuseManifest(const std::string& dscname, const std::string& dscbase);

    void Finalize(Version* v);

    void GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                  InternalKey* largest);

    void GetRange2(const std::vector<FileMetaData*>& inputs1,
                   const std::vector<FileMetaData*>& inputs2, InternalKey* smallest,
                   InternalKey* largest);

    void SetupOtherInputs(Compaction* c);

    // Save current contents to *log
    Status WriteSnapshot(log::Writer* log);

    void AppendVersion(Version* v);

    /* part 1: 由构造函数直接确定，属于 DB 运行时的基本信息 */
    Env* const env_;
    const std::string dbname_;
    const Options* const options_;
    TableCache* const table_cache_;
    const InternalKeyComparator icmp_;

    /* part 2: Meta Data，包括 SSTable Number、Log Number 以及上一个 SEQ 等信息
     */
    uint64_t next_file_number_;
    uint64_t manifest_file_number_;
    uint64_t last_sequence_;
    uint64_t log_number_;
    uint64_t prev_log_number_;  // 0 or backing store for memtable being compacted

    /* part 3: Opened lazily, manifest 相关 */
    // descriptor_file_ 指向当前正在使用的 MANIFEST 文件. 
    WritableFile* descriptor_file_;
    // descriptor_log_ 是 descriptor_file_ 的 Writer。
    log::Writer* descriptor_log_;

    /* part 4: Double Linked List */
    Version dummy_versions_;  // Head of circular doubly-linked list of versions.
    Version* current_;        // == dummy_versions_.prev_

    /* part 5: Compaction 相关
     *
     * Per-level key at which the next compaction at that level should start.
     * Either an empty string, or a valid InternalKey.
     *
     * 记录每一个 level 下一次 Compaction 的起始 InternalKey，可以为空字符串 */
    std::string compact_pointer_[config::kNumLevels];
};

// A Compaction encapsulates information about a compaction.
// 记录 Compaction 信息
class Compaction {
   public:
    ~Compaction();

    // Return the level that is being compacted.  Inputs from "level"
    // and "level+1" will be merged to produce a set of "level+1" files.
    int level() const { return level_; }

    // Return the object that holds the edits to the descriptor done
    // by this compaction.
    VersionEdit* edit() { return &edit_; }

    // "which" must be either 0 or 1
    int num_input_files(int which) const { return inputs_[which].size(); }

    // Return the ith input file at "level()+which" ("which" must be 0 or 1).
    FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

    // Maximum size of files to build during this compaction.
    uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

    // Is this a trivial compaction that can be implemented by just
    // moving a single input file to the next level (no merging or splitting)
    bool IsTrivialMove() const;

    // Add all inputs to this compaction as delete operations to *edit.
    void AddInputDeletions(VersionEdit* edit);

    // Returns true if the information we have available guarantees that
    // the compaction is producing data in "level+1" for which no data exists
    // in levels greater than "level+1".
    bool IsBaseLevelForKey(const Slice& user_key);

    // Returns true iff we should stop building the current output
    // before processing "internal_key".
    bool ShouldStopBefore(const Slice& internal_key);

    // Release the input version for the compaction, once the compaction
    // is successful.
    void ReleaseInputs();

   private:
    friend class Version;
    friend class VersionSet;

    /* 私有构造函数，但是由于 VersionSet 是友元类，所以可以在其成员方法中实例化
     * Compaction 对象 */
    Compaction(const Options* options, int level);

    /* 此次 Compaction 的起始 level，或者说，inputs 所在 level */
    int level_;
    uint64_t max_output_file_size_;
    Version* input_version_;
    VersionEdit edit_;

    // Each compaction reads inputs from "level_" and "level_+1"
    /* 核心字段
     * inputs_[0] 表示 level K 将要进行 Compact 的 sst files (vector)
     * inputs_[1] 表示 level K+1 将要进行 Compact 的 sst files (vector) */
    std::vector<FileMetaData*> inputs_[2];  // The two sets of inputs

    // State used to check for number of overlapping grandparent files
    // (parent == level_ + 1, grandparent == level_ + 2)
    std::vector<FileMetaData*> grandparents_;
    size_t grandparent_index_;  // Index in grandparent_starts_
    bool seen_key_;             // Some output key has been seen
    int64_t overlapped_bytes_;  // Bytes of overlap between current output
                                // and grandparent files

    // State for implementing IsBaseLevelForKey

    // level_ptrs_ holds indices into input_version_->levels_: our state
    // is that we are positioned at one of the file ranges for each
    // higher level than the ones involved in this compaction (i.e. for
    // all L >= level_ + 2).
    size_t level_ptrs_[config::kNumLevels];
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_SET_H_
