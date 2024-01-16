// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_set.h"

#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include <algorithm>
#include <cstdio>

#include "leveldb/env.h"
#include "leveldb/table_builder.h"

#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

static size_t TargetFileSize(const Options* options) { return options->max_file_size; }

// Maximum bytes of overlaps in grandparent (i.e., level+2) before we
// stop building a single file in a level->level+1 compaction.
static int64_t MaxGrandParentOverlapBytes(const Options* options) {
    return 10 * TargetFileSize(options);
}

// Maximum number of bytes in all compacted files.  We avoid expanding
// the lower level file set of a compaction if it would make the
// total compaction cover more than this many bytes.
static int64_t ExpandedCompactionByteSizeLimit(const Options* options) {
    return 25 * TargetFileSize(options);
}

static double MaxBytesForLevel(const Options* options, int level) {
    // Note: the result for level zero is not really used since we set
    // the level-0 compaction threshold based on number of files.

    /* 1048576 = 1024 * 1024，也就是说，第一层的阈值为 10M */
    double result = 10. * 1048576.0;
    while (level > 1) {
        result *= 10;
        level--;
    }
    return result;
}

static uint64_t MaxFileSizeForLevel(const Options* options, int level) {
    // We could vary per level to reduce number of files?
    return TargetFileSize(options);
}

static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
    int64_t sum = 0;
    for (size_t i = 0; i < files.size(); i++) {
        sum += files[i]->file_size;
    }
    return sum;
}

Version::~Version() {
    assert(refs_ == 0);

    // Remove from linked list
    prev_->next_ = next_;
    next_->prev_ = prev_;

    // Drop references to files
    for (int level = 0; level < config::kNumLevels; level++) {
        for (size_t i = 0; i < files_[level].size(); i++) {
            FileMetaData* f = files_[level][i];
            assert(f->refs > 0);
            f->refs--;
            if (f->refs <= 0) {
                delete f;
            }
        }
    }
}

// 二分查找，在 files 中找到第一个 largest_key >= key 的索引号 
int FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files,
             const Slice& key) {
    // 二分查找的左边界
    uint32_t left = 0;
    // 二分查找的右边界
    uint32_t right = files.size();
    while (left < right) {
        // 计算中间位置
        uint32_t mid = (left + right) / 2;
        const FileMetaData* f = files[mid];
        if (icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0) {
            // Key at "mid.largest" is < "target".  Therefore all
            // files at or before "mid" are uninteresting.
            // 如果 files[mid].largest < key，
            // 那么 files[0] ~ files[mid] 都不是我们要找的,
            // 可以直接排除 [0,mid] 的范围
            left = mid + 1;
        } else {
            // Key at "mid.largest" is >= "target".  Therefore all files
            // after "mid" are uninteresting.
            // 如果 files[mid].largest >= key，
            // 那么可以排除 [mid+1, right] 的范围
            right = mid;
        }
    }
    return right;
}

static bool AfterFile(const Comparator* ucmp, const Slice* user_key, const FileMetaData* f) {
    // null user_key occurs before all keys and is therefore never after *f
    return (user_key != nullptr && ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

static bool BeforeFile(const Comparator* ucmp, const Slice* user_key, const FileMetaData* f) {
    // null user_key occurs after all keys and is therefore never before *f
    return (user_key != nullptr && ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

/*
 * SomeFileOverlapsRange() 做的事情其实就是判断 New SSTable 和当前 level
 * 是否存在重叠的 Key。
 *
 * disjoint_sorted_files 表示是否互斥，只有在计算 level 0 时该值为
 * false，其余情况为 true; files 为每一层的 FileMetaData 记录，包括 level 0 层
 * */
// 用于判断输入的 sst 集合(files)中,
// 是否存在 sst 与给定的 key 范围([smallest_user_key, largest_user_key])有重叠。
//
// 其中的 disjoint_sorted_files 参数表示 files 中各个 file 是否互不重叠。
// 如果 files 是属于 level-0 的，那 disjoint_sorted_files 需要为 false，
// 因为 level-0 的 sst files 不能保证互相不重叠。
// 对于其他 level 的 sst files， disjoint_sorted_files 为 true。
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp, bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files, const Slice* smallest_user_key,
                           const Slice* largest_user_key) {
    // 通过比较 User Key 来判断是否存在 overlap，
    // 所以要用到 User Comparator。
    const Comparator* ucmp = icmp.user_comparator();

    // 对于 level-0 的 sst files，需要将输入 files 挨个遍历一遍，
    // 看下是否存在 overlap。
    if (!disjoint_sorted_files) {
        // Need to check against all files
        for (size_t i = 0; i < files.size(); i++) {
            const FileMetaData* f = files[i];

            // 如果 smallest_user_key 比 file 的所有 key 都要大，
            // 或者 largest_user_key 比 file 的所有 key 都要小，
            // 就没有 overlap。
            if (AfterFile(ucmp, smallest_user_key, f) || BeforeFile(ucmp, largest_user_key, f)) {
                // No overlap
            } else {
                return true;  // Overlap
            }
        }
        return false;
    }

    // Binary search over file list
    // index 表示可能与 [smallest_user_key, largest_user_key] 有 overlap 的 files[index]
    uint32_t index = 0;
    // 如果 smallest_user_key 为 nullptr，表示 smallest_user_key 为无限小，
    // 那唯一有可能与 [smallest_user_key, largest_user_key] 有 overlap 的 file 为 files[0]。
    if (smallest_user_key != nullptr) {
        // Find the earliest possible internal key for smallest_user_key
        // smallest_user_key 不为 nullptr的话，就用二分查找的方式找到第一个
        // 有可能与 [small_user_key, largest_user_key] 有 overlap 的 file[index]。

        // 至于为什么这里要用将 small_key 的 seq 设为 kMaxSequenceNumber，
        // 因为两个 InternalKey 之间的排序规则是:
        //   - User Key 升序
        //   - Sequence Number 降序
        //   - Value Type 降序
        // 所以 InternalKey small_key(*smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek) 
        // 在面对一个携带相同 User Key 的 InternalKey 时，small_key 一定排在前面。
        InternalKey small_key(*smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);

        // 二分查找，找到第一个 largest_key >= small_key 的 files[index]。
        index = FindFile(icmp, files, small_key.Encode());
    }

    // 如果 index 越界了，说明 smallest_user_key 比 files 中所有的 key 都要大，
    // 一定不存在 overlap。
    if (index >= files.size()) {
        // beginning of range is after all files, so no overlap.
        return false;
    }

    // 已知 smallest_user_key <= files[index].largest，
    // 如果 largest_user_key 比 files[index].smallest 还要小的话，
    // 也一定不存在 overlap。
    // 否则一定有 overlap。
    return !BeforeFile(ucmp, largest_user_key, files[index]);
}

// An internal iterator.  For a given version/level pair, yields
// information about the files in the level.  For a given entry, key()
// is the largest key that occurs in the file, and value() is an
// 16-byte value containing the file number and file size, both
// encoded using EncodeFixed64.
class Version::LevelFileNumIterator : public Iterator {
   public:
    LevelFileNumIterator(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>* flist)
        : icmp_(icmp), flist_(flist), index_(flist->size()) {  // Marks as invalid
    }
    bool Valid() const override { return index_ < flist_->size(); }
    void Seek(const Slice& target) override { index_ = FindFile(icmp_, *flist_, target); }
    void SeekToFirst() override { index_ = 0; }
    void SeekToLast() override { index_ = flist_->empty() ? 0 : flist_->size() - 1; }
    void Next() override {
        assert(Valid());
        index_++;
    }
    void Prev() override {
        assert(Valid());
        if (index_ == 0) {
            index_ = flist_->size();  // Marks as invalid
        } else {
            index_--;
        }
    }
    Slice key() const override {
        assert(Valid());
        return (*flist_)[index_]->largest.Encode();
    }
    Slice value() const override {
        assert(Valid());
        EncodeFixed64(value_buf_, (*flist_)[index_]->number);
        EncodeFixed64(value_buf_ + 8, (*flist_)[index_]->file_size);
        return Slice(value_buf_, sizeof(value_buf_));
    }
    Status status() const override { return Status::OK(); }

   private:
    const InternalKeyComparator icmp_;
    const std::vector<FileMetaData*>* const flist_;
    uint32_t index_;

    // Backing store for value().  Holds the file number and size.
    mutable char value_buf_[16];
};

static Iterator* GetFileIterator(void* arg, const ReadOptions& options, const Slice& file_value) {
    TableCache* cache = reinterpret_cast<TableCache*>(arg);
    if (file_value.size() != 16) {
        return NewErrorIterator(Status::Corruption("FileReader invoked with unexpected value"));
    } else {
        return cache->NewIterator(options, DecodeFixed64(file_value.data()),
                                  DecodeFixed64(file_value.data() + 8));
    }
}

Iterator* Version::NewConcatenatingIterator(const ReadOptions& options, int level) const {
    return NewTwoLevelIterator(new LevelFileNumIterator(vset_->icmp_, &files_[level]),
                               &GetFileIterator, vset_->table_cache_, options);
}

void Version::AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) {
    // Merge all level zero files together since they may overlap
    // 对于 level-0 的 SST，会有 SST 重叠的情况，所以需要每个 SST 的 iterator。
    for (size_t i = 0; i < files_[0].size(); i++) {
        iters->push_back(vset_->table_cache_->NewIterator(options, files_[0][i]->number,
                                                          files_[0][i]->file_size));
    }

    // For levels > 0, we can use a concatenating iterator that sequentially
    // walks through the non-overlapping files in the level, opening them
    // lazily.
    // 对于 level > 0 的 SST，不会有 SST 重叠的情况，所以可以使用一个 ConcatenatingIterator，
    // 就是将一个 level 上的所有 SST 的 iterator 拼接起来，作为一个 iterator 使用。
    for (int level = 1; level < config::kNumLevels; level++) {
        if (!files_[level].empty()) {
            iters->push_back(NewConcatenatingIterator(options, level));
        }
    }
}

// Callback from TableCache::Get()
namespace {
enum SaverState {
    kNotFound,
    kFound,
    kDeleted,
    kCorrupt,
};
struct Saver {
    SaverState state;
    const Comparator* ucmp;
    Slice user_key;
    std::string* value;
};
}  // namespace
static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
    Saver* s = reinterpret_cast<Saver*>(arg);
    ParsedInternalKey parsed_key;
    if (!ParseInternalKey(ikey, &parsed_key)) {
        s->state = kCorrupt;
    } else {
        if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
            s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
            if (s->state == kFound) {
                s->value->assign(v.data(), v.size());
            }
        }
    }
}

static bool NewestFirst(FileMetaData* a, FileMetaData* b) { return a->number > b->number; }

void Version::ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                                 bool (*func)(void*, int, FileMetaData*)) {
    const Comparator* ucmp = vset_->icmp_.user_comparator();

    // Search level-0 in order from newest to oldest.
    // 遍历 level-0 中的 SST 文件，将与 user_key 有重叠的 SST 文件放到 tmp 中。
    std::vector<FileMetaData*> tmp;
    tmp.reserve(files_[0].size());
    for (uint32_t i = 0; i < files_[0].size(); i++) {
        FileMetaData* f = files_[0][i];
        // 如果 user_key >= f->smallest.user_key() && user_key <= f->largest.user_key()，
        // 将该 SST 文件放到 tmp 中。
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
            ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
            tmp.push_back(f);
        }
    }
    if (!tmp.empty()) {
        // 将 tmp 里的 SST 按照从新到旧的顺序排序，
        // 再遍历 tmp，挨个调用 func(arg, 0, tmp[i])。
        std::sort(tmp.begin(), tmp.end(), NewestFirst);
        for (uint32_t i = 0; i < tmp.size(); i++) {
            if (!(*func)(arg, 0, tmp[i])) {
                return;
            }
        }
    }

    // Search other levels.
    // 到其他 level 中寻找与 user_key 有重叠的 SST 文件，调用 func(arg, level, f)。
    for (int level = 1; level < config::kNumLevels; level++) {
        size_t num_files = files_[level].size();
        if (num_files == 0) continue;

        // Binary search to find earliest index whose largest key >=
        // internal_key.
        // 用二分查找的方式，在该 level 中找到第一个 largest_key >= internal_key 的 SST 文件。
        uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
        if (index < num_files) {
            FileMetaData* f = files_[level][index];
            if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
                // All of "f" is past any data for user_key
                // 如果 user_key < f->smallest.user_key()，说明 user_key 与 f 没有重叠，
            } else {
                // user_key >= f->smallest.user_key()，user_key 与 f 有重叠，
                // 调用 func(arg, level, f)。
                if (!(*func)(arg, level, f)) {
                    return;
                }
            }
        }
    }
}

Status Version::Get(const ReadOptions& options, const LookupKey& k, std::string* value,
                    GetStats* stats) {
    stats->seek_file = nullptr;
    stats->seek_file_level = -1;

    struct State {
        // Saver 结构体包含：
        //   - SaverState state;：表示 Saver 的状态，可能的值包括 kNotFound、kFound、kDeleted、 kCorrupt
        //   - const Comparator* ucmp;：user key comparator
        //   - Slice user_key;：user key
        //   - std::string* value;：用于存储找到的 value
        Saver saver;

        // stats 用于记录 Get 操作的一些额外返回信息:
        //   - key 所在 SST 的 MetaData
        //   - key 所在 SST 的 level
        GetStats* stats;

        // 一些影响读取 SST 的选项，比如是否要验证 checksums: options.verify_checksums
        const ReadOptions* options;

        // k 所对应的 internal_key
        Slice ikey;

        // 最后读取的 SST 文件
        FileMetaData* last_file_read;
        // 最后读取的 SST 文件所在的 level
        int last_file_read_level;

        // 包含了 leveldb 所有 version 的 VersionSet
        VersionSet* vset;

        // 存储 Match 方法中调用 TableCache::Get 时返回的 Status
        Status s;
        // 用于标识是否找到了 key
        bool found;

        static bool Match(void* arg, int level, FileMetaData* f) {
            State* state = reinterpret_cast<State*>(arg);

            // 记录第一个被查找的 SST 文件，及其所在 level。
            if (state->stats->seek_file == nullptr && state->last_file_read != nullptr) {
                // We have had more than one seek for this read.  Charge the 1st
                // file.
                // state->stats->seek_file == nullptr 表示第一个被查找的 SST 还没有被记录，
                // state->last_file_read != nullptr 表示该轮 Match 不是第一次被调用。
                state->stats->seek_file = state->last_file_read;
                state->stats->seek_file_level = state->last_file_read_level;
            }

            // 记录最后一个被查找的 SST 文件，及其所在 level。
            state->last_file_read = f;
            state->last_file_read_level = level;

            // 借助 table_cache_ 在当前 SST 中查找 ikey，
            // 如果查找到了，就调用 SaveValue(&state->saver, found_key, found_value) 方法，
            // 将 found_value 存储到 state->saver.value 中。
            state->s = state->vset->table_cache_->Get(*state->options, f->number, f->file_size,
                                                      state->ikey, &state->saver, SaveValue);
            
            // 如果在当前 SST 中查找到 key 了，则设置 state->found 为 true，
            // 并且返回 false，表示不需要继续到其他 SST 中查找了。
            // 因为 Match 方法是依照 Latest SST 到 Oldest SST 的顺序遍历调用的，
            // 如果在 Latest SST 中找到了目标 key，Older SST 中的 key 就是无效的旧值了。
            if (!state->s.ok()) {
                state->found = true;
                return false;
            }

            // 如果在当前 SST 中查找 key 失败，需要根据失败的原因做进一步处理。
            switch (state->saver.state) {
                case kNotFound:
                    // 当前 SST 中没有找到 key，
                    // 返回 true 表示继续到其他 SST 中查找。
                    return true;  // Keep searching in other files
                case kFound:
                    // 当前 SST 中找到了 key，但是处理过程中出错了，无法获取到对应的 value，
                    // 返回 false 表示不需要继续到其他 SST 中查找了。
                    state->found = true;
                    return false;
                case kDeleted:
                    // 当前 SST 中找到了 key，但是 key 已经被删除了，
                    // 返回 false 表示不需要继续到其他 SST 中查找了。
                    return false;
                case kCorrupt:
                    // 查找的过程中 crash 了，
                    // 返回 false 表示不需要继续到其他 SST 中查找了。
                    state->s = Status::Corruption("corrupted key for ", state->saver.user_key);
                    state->found = true;
                    return false;
            }

            // Not reached. Added to avoid false compilation warnings of
            // "control reaches end of non-void function".
            // 不会执行到这里，添加一个 return 只是为了避免编译器报错。
            return false;
        }
    };

    State state;
    state.found = false;
    state.stats = stats;
    state.last_file_read = nullptr;
    state.last_file_read_level = -1;

    state.options = &options;
    state.ikey = k.internal_key();
    state.vset = vset_;

    state.saver.state = kNotFound;
    state.saver.ucmp = vset_->icmp_.user_comparator();
    state.saver.user_key = k.user_key();
    state.saver.value = value;

    // 遍历所有与 user_key 有重叠的 SST 文件，
    // 依照 Latest SST 到 Oldest SST 的顺序，挨个对每个 SST 调用 State::Match(&state, level, fileMetaData) 方法。
    // 如果 State::Match 返回 true，则停止调用。
    ForEachOverlapping(state.saver.user_key, state.ikey, &state, &State::Match);

    return state.found ? state.s : Status::NotFound(Slice());
}

bool Version::UpdateStats(const GetStats& stats) {
    FileMetaData* f = stats.seek_file;
    if (f != nullptr) {
        // 将 SST 的 allowed_seeks 减 1，
        // 当 allowed_seeks 减到 0 时，就将该 SST 加入到 compact 调度中。
        f->allowed_seeks--;
        if (f->allowed_seeks <= 0 && file_to_compact_ == nullptr) {
            file_to_compact_ = f;
            file_to_compact_level_ = stats.seek_file_level;
            return true;
        }
    }
    return false;
}

bool Version::RecordReadSample(Slice internal_key) {

    // 从 internal_key 中提取出 user_key
    ParsedInternalKey ikey;
    if (!ParseInternalKey(internal_key, &ikey)) {
        return false;
    }

    struct State {
        // stats 记录第一个包含 user_key的 SST 文件
        GetStats stats;  // Holds first matching file
        // matches 记录包含 user_key 的 SST 文件的个数
        int matches;

        static bool Match(void* arg, int level, FileMetaData* f) {
            State* state = reinterpret_cast<State*>(arg);
            state->matches++;
            if (state->matches == 1) {
                // Remember first match.
                state->stats.seek_file = f;
                state->stats.seek_file_level = level;
            }
            // We can stop iterating once we have a second match.
            return state->matches < 2;
        }
    };

    State state;
    state.matches = 0;
    // 遍历所有与 user_key 有重叠的 SST 文件，调用 State::Match 方法。
    ForEachOverlapping(ikey.user_key, internal_key, &state, &State::Match);

    // Must have at least two matches since we want to merge across
    // files. But what if we have a single file that contains many
    // overwrites and deletions?  Should we have another mechanism for
    // finding such files?
    // 如果至少有 2 个 SST 包含了 user_key，调用 UpdateStats 更新一下
    // 第一个 SST 的 allowed_seeks。
    if (state.matches >= 2) {
        // 1MB cost is about 1 seek (see comment in Builder::Apply).
        return UpdateStats(state.stats);
    }
    return false;
}

void Version::Ref() { ++refs_; }

void Version::Unref() {
    assert(this != &vset_->dummy_versions_);
    assert(refs_ >= 1);
    --refs_;
    if (refs_ == 0) {
        delete this;
    }
}

bool Version::OverlapInLevel(int level, const Slice* smallest_user_key,
                             const Slice* largest_user_key) {
    return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level], smallest_user_key,
                                 largest_user_key);
}

// PickLevelForMemTableOutput 方法的作用是选择新的SSTable应该放置在哪一层。
// 它首先检查新的SSTable的键范围是否与第0层有重叠，如果没有，
// 那么尝试将新的SSTable放置在更高的层级，直到遇到与新的SSTable的键范围有重叠的层级，
// 或者新的SSTable的键范围与下两层的文件重叠的总大小超过限制。
int Version::PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                        const Slice& largest_user_key) {
    int level = 0;

    // 如果 New SST 和 level 0 层有重叠的话，那只能选择 level 0 层。
    // 否则的话，就继续往更大的 level 找，直到找到第一个和 New SST 有重叠的 level。
    if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
        // Push to next level if there is no overlap in next level,
        // and the #bytes overlapping in the level after that are limited.
        // 为啥要这样构造 start 和 limit ?
        InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
        InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
        std::vector<FileMetaData*> overlaps;

        // kMaxMemCompactLevel 默认为 2。
        while (level < config::kMaxMemCompactLevel) {
            // 如果高一层的 level+1 与 New SST 有重叠了，就不要继续试探了，
            // 使用 level 层就行。
            if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
                break;
            }

            // 如果 level+2 层存在，还要检查下 New SST 和 level+2 层
            // 重叠的大小是否超过了阈值。
            if (level + 2 < config::kNumLevels) {
                // Check that file does not overlap too many grandparent bytes.
                // 检查 New SST 与 level+2 层的哪些 SST 有重叠，
                // 这些重叠的 SST 存储在 std::vector<FileMetaData*> overlaps 中。
                GetOverlappingInputs(level + 2, &start, &limit, &overlaps);

                /* 这里是一个预估值，因为文件上的 key 不可能是均匀分布的 */
                // 计算 level+2 中，与 New SST 重叠的 SST 大小总和。
                // 如果这个总和超过了阈值，就不能使用 level+1，只能使用 level。
                const int64_t sum = TotalFileSize(overlaps);
                if (sum > MaxGrandParentOverlapBytes(vset_->options_)) {
                    break;
                }
            }
            level++;
        }
    }
    return level;
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
// 在 level 层中查找有哪些 SST 文件与 [begin,end] 有重叠，
// 将他们存储在 inputs 中返回。
void Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end,
                                   std::vector<FileMetaData*>* inputs) {
    assert(level >= 0);
    assert(level < config::kNumLevels);
    inputs->clear();

    // 将 [begin, end] 转为 [user_key_begin, user_key_end]
    Slice user_begin, user_end;
    if (begin != nullptr) {
        user_begin = begin->user_key();
    }
    if (end != nullptr) {
        user_end = end->user_key();
    }

    // 获取 user key comparator
    const Comparator* user_cmp = vset_->icmp_.user_comparator();

    // 遍历 level 层中的所有 SST 文件
    for (size_t i = 0; i < files_[level].size();) {

        // 获取当前 SST 文件的 FileMetaData，
        // 提取出当前 SST 文件的 smallest user key 和 largest user key。
        FileMetaData* f = files_[level][i++];
        const Slice file_start = f->smallest.user_key();
        const Slice file_limit = f->largest.user_key();

        if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {
            // "f" is completely before specified range; skip it
            // 当前 SST 的 largest_user_key 比 user_begin 还要小，可以跳过当前 SST 了
        } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
            // "f" is completely after specified range; skip it
            // 当前 SST 的 smallest_user_key 比 user_end 还要大，可以跳过当前 SST 了
        } else {
            // 当前 SST 和 [user_begin, user_end] 有交集，将其加入 inputs 中
            inputs->push_back(f);

            
            if (level == 0) {
                // Level-0 files may overlap each other.  So check if the newly
                // added file has expanded the range.  If so, restart search.
                // level-0 层的 SST 文件可能会互相重叠，所以需要检查下是否存在间接重叠的情况: 
                //   [user_begin, user_end] 与 files_[0][i] 直接重叠，files[0][i] 和 files_[0][i-1] 直接重叠，
                //   导致 [user_begin, user_end] 与 files_[0][i-1] 虽然不直接重叠，但是间接重叠。
                // 间接重叠，也算 overlap。 
                // 所以对于 level-0，只要发现了 [user_begin, user_end] 与 files_[0][i] 有重叠，就需要
                // 更新 user_begin 和 user_end，将 i 置为 0，从 files_[0][0] 开始重新检测。
                if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
                    // 如果 files_[0] 与 [user_begin, user_end] 有重叠，并且 files_[0] 的 smallest_user_key 比 user_begin 还要小，
                    // 需要把 user_begin 更新为 files_[0] 的 smallest_user_key，从头开始重新检测 overlap。
                    user_begin = file_start;
                    inputs->clear();
                    i = 0;
                } else if (end != nullptr && user_cmp->Compare(file_limit, user_end) > 0) {
                    // 如果 files_[0] 与 [user_begin, user_end] 有重叠，并且 files_[0] 的 largest_user_key 比 user_end 还要大，
                    // 需要把 user_end 更新为 files_[0] 的 largest_user_key，从头开始重新检测 overlap。
                    user_end = file_limit;
                    inputs->clear();
                    i = 0;
                }
            }
        }
    }
}

std::string Version::DebugString() const {
    std::string r;
    for (int level = 0; level < config::kNumLevels; level++) {
        // E.g.,
        //   --- level 1 ---
        //   17:123['a' .. 'd']
        //   20:43['e' .. 'g']
        // 对于每个层级，首先添加一个表示层级的字符串，例如:
        //   "--- level 1 ---"。
        // 对于该层级中的每个文件，添加一个描述文件的字符串: 
        //   "{文件编号}:{文件大小}[smallest_key .. largest_key]"
        r.append("--- level ");
        AppendNumberTo(&r, level);
        r.append(" ---\n");
        const std::vector<FileMetaData*>& files = files_[level];
        for (size_t i = 0; i < files.size(); i++) {
            r.push_back(' ');
            AppendNumberTo(&r, files[i]->number);
            r.push_back(':');
            AppendNumberTo(&r, files[i]->file_size);
            r.append("[");
            r.append(files[i]->smallest.DebugString());
            r.append(" .. ");
            r.append(files[i]->largest.DebugString());
            r.append("]\n");
        }
    }
    return r;
}

/*
 * A helper class so we can efficiently apply a whole sequence
 * of edits to a particular state without creating intermediate
 * Versions that contain full copies of the intermediate state.
 *
 * Builder 的作用有点儿类似于"压缩机"。假如说我们现在有一个最初的 Version 和多个
 * VersionEdit， 那么如果我们想要得到最新的 Version 的话，就需要把所有的
 * VersionEdit 逐一应用在 Version 中。 Builder
 * 就是干这件事情的，并且不会产生大量的中间 Version。
 *
 * Builder 更像是一个工具类，所以我个人认为它不应该放在 VersionSet
 * 这个类中，但是不放在 VersionSet
 * 中的话很多方式实现起来就很麻烦，也算是一个设计上的妥协。
 * */
class VersionSet::Builder {
   private:
    // Helper to sort by v->files_[file_number].smallest
    //
    // 自定义 FileSet 的比较函数，按照 smallest key 升序排列。
    struct BySmallestKey {
        const InternalKeyComparator* internal_comparator;

        bool operator()(FileMetaData* f1, FileMetaData* f2) const {
            int r = internal_comparator->Compare(f1->smallest, f2->smallest);
            if (r != 0) {
                return (r < 0);
            } else {
                // Break ties by file number
                return (f1->number < f2->number);
            }
        }
    };

    /* 这里使用了 rbtree 所实现的 set，并且自定义了比较函数，FileSet.begin()
     * 就是整个文件集合中 smallest 最小的那个 FileMetaData */
    typedef std::set<FileMetaData*, BySmallestKey> FileSet;

    struct LevelState {
        std::set<uint64_t> deleted_files;
        FileSet* added_files;
    };

    /* vset_ 和 base_ 均由构造函数初始化 */
    VersionSet* vset_;
    Version* base_;

    /* levels_ 中记录了每一个 level 中的文件新增、删除情况 */
    LevelState levels_[config::kNumLevels];

   public:
    /* Initialize a builder with the files from *base and other info from *vset
     * 构造函数接收一个 VersionSet 和 Base Version */
    Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
        base_->Ref();
        BySmallestKey cmp;
        cmp.internal_comparator = &vset_->icmp_;

        // 为每一层 level 初始化 FileSet
        for (int level = 0; level < config::kNumLevels; level++) {
            levels_[level].added_files = new FileSet(cmp);
        }
    }

    ~Builder() {
        // 析构 levels_ 及其子元素
        for (int level = 0; level < config::kNumLevels; level++) {
            const FileSet* added = levels_[level].added_files;
            std::vector<FileMetaData*> to_unref;
            to_unref.reserve(added->size());
            for (FileSet::const_iterator it = added->begin(); it != added->end(); ++it) {
                to_unref.push_back(*it);
            }
            delete added;
            for (uint32_t i = 0; i < to_unref.size(); i++) {
                FileMetaData* f = to_unref[i];
                f->refs--;
                if (f->refs <= 0) {
                    delete f;
                }
            }
        }
        base_->Unref();
    }

    // Apply all of the edits in *edit to the current state.
    void Apply(VersionEdit* edit) {
        /* Update compaction pointers，更新 compact_pointers_ 中的内容，
         * compact_pointer_ 中的内容将直接决定 Compaction 时到底选取哪些
         * SSTable， 下面一段代码做的事情只是简单地把
         * VersionEdit::compact_pointers_ 中的内容转换成
         * VersionSet::compact_pointer_ 而已 */
        // vset_->compact_pointer_[i] 记录 level-i 下一次 Compaction 的起始 InternalKey。
        // 下面这段 for 循环只是把 VersionEdit::compact_pointers_ 中的内容转换成 VersionSet::compact_pointer_。
        for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
            const int level = edit->compact_pointers_[i].first;
            // vset_->compact_pointer_[i] 记录 level-i 下一次 Compaction 的起始 InternalKey
            vset_->compact_pointer_[level] = edit->compact_pointers_[i].second.Encode().ToString();
        }

        // Delete files
        //
        // 将 deleted files 记录到 levels_ 中。
        for (const auto& deleted_file_set_kvp : edit->deleted_files_) {
            const int level = deleted_file_set_kvp.first;
            const uint64_t number = deleted_file_set_kvp.second;
            levels_[level].deleted_files.insert(number);
        }

        // Add new files
        //
        // 将 new files 记录到 levels_ 中。
        for (size_t i = 0; i < edit->new_files_.size(); i++) {
            const int level = edit->new_files_[i].first;
            FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
            f->refs = 1;

            // We arrange to automatically compact this file after
            // a certain number of seeks.  Let's assume:
            //   (1) One seek costs 10ms
            //   (2) Writing or reading 1MB costs 10ms (100MB/s)
            //   (3) A compaction of 1MB does 25MB of IO:
            //         1MB read from this level
            //         10-12MB read from next level (boundaries may be
            //         misaligned) 10-12MB written to next level
            // This implies that 25 seeks cost the same as the compaction
            // of 1MB of data.  I.e., one seek costs approximately the
            // same as the compaction of 40KB of data.  We are a little
            // conservative and allow approximately one seek for every 16KB
            // of data before triggering a compaction.
            // 设置每个 New SST 文件的 allowed_seeks。
            // allowed_seeks 表示该 SST 文件允许被查找的次数，每被读取一次，allowed_seeks 就减 1。
            // allowed_seeks 为 0 时，该 SST 文件就会被加入到 compaction 调度中。
            // allowed_seeks 的计算方式如下: max(100, file_size / 16KB)。
            f->allowed_seeks = static_cast<int>((f->file_size / 16384U));
            if (f->allowed_seeks < 100) f->allowed_seeks = 100;

            levels_[level].deleted_files.erase(f->number);
            levels_[level].added_files->insert(f);
        }
    }

    // Save the current state in *v.
    void SaveTo(Version* v) {
        BySmallestKey cmp;
        cmp.internal_comparator = &vset_->icmp_;
        for (int level = 0; level < config::kNumLevels; level++) {
            // Merge the set of added files with the set of pre-existing files.
            // Drop any deleted files.  Store the result in *v.
            //
            // base_files 是该层上原有的 SST，added_files 是该层上新增的 SST。
            // 将 base_files 与 added_files 合并后，存放到 v->files_[level] 中。
            //
            // 对于每一层，先准备好 base_files 和 added_files。
            // 比如说，base_files = [1, 3, 5]，added_files = [2, 4]
            // 那么就应该按照 [1, 2, 3, 4, 5] 的顺序调用 MaybeAddFile 方法，将 SST 添加
            // 到 v->files_[level] 中。
            const std::vector<FileMetaData*>& base_files = base_->files_[level];
            std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
            std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
            const FileSet* added_files = levels_[level].added_files;
            v->files_[level].reserve(base_files.size() + added_files->size());
            for (const auto& added_file : *added_files) {
                // Add all smaller files listed in base_
                for (std::vector<FileMetaData*>::const_iterator bpos =
                         std::upper_bound(base_iter, base_end, added_file, cmp);
                     base_iter != bpos; ++base_iter) {
                    MaybeAddFile(v, level, *base_iter);
                }

                MaybeAddFile(v, level, added_file);
            }

            // Add remaining base files
            for (; base_iter != base_end; ++base_iter) {
                MaybeAddFile(v, level, *base_iter);
            }

#ifndef NDEBUG
            // Make sure there is no overlap in levels > 0
            if (level > 0) {
                for (uint32_t i = 1; i < v->files_[level].size(); i++) {
                    const InternalKey& prev_end = v->files_[level][i - 1]->largest;
                    const InternalKey& this_begin = v->files_[level][i]->smallest;
                    if (vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
                        std::fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                                     prev_end.DebugString().c_str(),
                                     this_begin.DebugString().c_str());
                        std::abort();
                    }
                }
            }
#endif
        }
    }

    void MaybeAddFile(Version* v, int level, FileMetaData* f) {
        if (levels_[level].deleted_files.count(f->number) > 0) {
            // File is deleted: do nothing
            // 
            // 如果 f 出现在 deleted_files 中，说明该 SST 文件已经被删除了，不需要添加到 v->files_[level] 中。
        } else {
            // 如果 f 没有出现在 deleted_files 中，则将其添加到 v->files_[level] 中。
            std::vector<FileMetaData*>* files = &v->files_[level];
            if (level > 0 && !files->empty()) {
                // Must not overlap
                assert(vset_->icmp_.Compare((*files)[files->size() - 1]->largest, f->smallest) < 0);
            }
            f->refs++;
            files->push_back(f);
        }
    }
};

VersionSet::VersionSet(const std::string& dbname, const Options* options, TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      next_file_number_(2),
      manifest_file_number_(0),  // Filled by Recover()
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      descriptor_file_(nullptr),
      descriptor_log_(nullptr),
      dummy_versions_(this),
      current_(nullptr) {
    AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
    current_->Unref();
    assert(dummy_versions_.next_ == &dummy_versions_);  // List must be empty
    delete descriptor_log_;
    delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v) {
    // Make "v" current
    assert(v->refs_ == 0);
    assert(v != current_);
    if (current_ != nullptr) {
        current_->Unref();
    }
    current_ = v;
    v->Ref();

    // Append to linked list
    v->prev_ = dummy_versions_.prev_;
    v->next_ = &dummy_versions_;
    v->prev_->next_ = v;
    v->next_->prev_ = v;
}

Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu) {
    // has_log_number_ 表示该 edit 是否有对应的 WAL
    // log_number_ 为 edit 对应的 WAL 编号
    if (edit->has_log_number_) {
        // 如果该 edit 是否有对应的 WAL，
        // 检查其 WAL 编号是否合法。
        assert(edit->log_number_ >= log_number_);
        assert(edit->log_number_ < next_file_number_);
    } else {
        // 如果该 edit 没有对应的 WAL，
        // 那么就使用当前的 WAL 编号。
        edit->SetLogNumber(log_number_);
    }

    // 如果该 edit 没有对应的 prev WAL 编号，
    // 那么就使用当前的 prev WAL 编号。
    if (!edit->has_prev_log_number_) {
        edit->SetPrevLogNumber(prev_log_number_);
    }

    // 将 next_file_number_ 和 last_sequence_ 保存到 edit 中，
    // 这样 apply edit 的时候，new version 才能包含这两个值。
    edit->SetNextFile(next_file_number_);
    edit->SetLastSequence(last_sequence_);

    // 创建一个新的 Version，利用 Builder，基于当前版本 current_，
    // 把 edit 应用到新的 Version 中。
    // 简单来说: current_ + edit = new version
    Version* v = new Version(this);
    {
        Builder builder(this, current_);
        builder.Apply(edit);
        builder.SaveTo(v);
    }
    Finalize(v);

    // Initialize new descriptor log file if necessary by creating
    // a temporary file that contains a snapshot of the current version.
    // 每产生一个新的 Version，都需要创建一个对应的 New MANIFEST。
    std::string new_manifest_file;
    Status s;
    // 当数据库是第一次被打开时，descriptor_log_ 为 nullptr。
    if (descriptor_log_ == nullptr) {
        // No reason to unlock *mu here since we only hit this path in the
        // first call to LogAndApply (when opening the database).
        assert(descriptor_file_ == nullptr);
        // 生成新的 MANIFEST 文件名
        new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);

        // 此处的 edit->SetNextFile(next_file_number_); 是多余的，
        // 之前已经 set 过了。
        edit->SetNextFile(next_file_number_);

        // 由于数据库是第一次打开，需要先把当前状态写入到新的 MANIFEST 文件中，作为 base version。
        // 状态指的是每层 Level 都有哪些 SST 文件。
        s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
        if (s.ok()) {
            descriptor_log_ = new log::Writer(descriptor_file_);
            // 将数据库的当前状态写入到新的 MANIFEST 文件中。
            s = WriteSnapshot(descriptor_log_);
        }
    }

    // Unlock during expensive MANIFEST log write
    {
        // 只有以下 3 种情况会读取 MANIFEST 和 CURRETN 文件:
        //   - 数据库启动时
        //   - Recover 时
        //   - Compaction 后调用 VersionSet::LogAndApply
        // 此处可以 unlock mu，是因为上面这 3 种情况不会有 2 种同时发生。
        mu->Unlock();

        // Write new record to MANIFEST log
        // 往 MANIFEST 中添加 edit。
        if (s.ok()) {
            std::string record;
            edit->EncodeTo(&record);
            s = descriptor_log_->AddRecord(record);
            if (s.ok()) {
                s = descriptor_file_->Sync();
            }
            if (!s.ok()) {
                Log(options_->info_log, "MANIFEST write: %s\n", s.ToString().c_str());
            }
        }

        // If we just created a new descriptor file, install it by writing a
        // new CURRENT file that points to it.
        // 如果 MANIFEST 是新创建的，还需要更新 CURRENT。
        if (s.ok() && !new_manifest_file.empty()) {
            s = SetCurrentFile(env_, dbname_, manifest_file_number_);
        }

        mu->Lock();
    }

    // Install the new version
    if (s.ok()) {
        // 将生成的新 Version 加入 VersionSet。
        AppendVersion(v);
        log_number_ = edit->log_number_;
        prev_log_number_ = edit->prev_log_number_;
    } else {
        delete v;
        if (!new_manifest_file.empty()) {
            delete descriptor_log_;
            delete descriptor_file_;
            descriptor_log_ = nullptr;
            descriptor_file_ = nullptr;
            env_->RemoveFile(new_manifest_file);
        }
    }

    return s;
}

// 从数据库中读取 CURRENT 文件，解析 MANIFEST文件，
// 恢复数据库的状态(每层 Level 都有哪些 SST 文件)。
Status VersionSet::Recover(bool* save_manifest) {
    struct LogReporter : public log::Reader::Reporter {
        Status* status;
        void Corruption(size_t bytes, const Status& s) override {
            if (this->status->ok()) *this->status = s;
        }
    };

    // Read "CURRENT" file, which contains a pointer to the current manifest
    // file
    //
    // 读取 CURRENT 文件的内容，存放到 std::string current 中。
    std::string current;
    Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
    if (!s.ok()) {
        return s;
    }
    // 如果 CURRENT 文件为空，或者没有以 '\n' 结尾，说明 CURRENT 文件有问题，
    // 返回失败。
    if (current.empty() || current[current.size() - 1] != '\n') {
        return Status::Corruption("CURRENT file does not end with newline");
    }
    // 去掉 current 中的 '\n'。
    current.resize(current.size() - 1);

    // current 中存放的是 MANIFEST 的文件名，例如 MANIFEST-000001。
    // dscname 为 MANIFEST 的完整路径。
    std::string dscname = dbname_ + "/" + current;
    SequentialFile* file;
    // 以顺序读取的方式打开 MANIFEST 文件。
    s = env_->NewSequentialFile(dscname, &file);
    if (!s.ok()) {
        if (s.IsNotFound()) {
            return Status::Corruption("CURRENT points to a non-existent file", s.ToString());
        }
        return s;
    }

    bool have_log_number = false;
    bool have_prev_log_number = false;
    bool have_next_file = false;
    bool have_last_sequence = false;
    uint64_t next_file = 0;
    uint64_t last_sequence = 0;
    uint64_t log_number = 0;
    uint64_t prev_log_number = 0;
    Builder builder(this, current_);
    int read_records = 0;

    {
        // 构造一个 reader，用于读取 MANIFEST 文件。
        LogReporter reporter;
        reporter.status = &s;
        log::Reader reader(file, &reporter, true /*checksum*/, 0 /*initial_offset*/);
        Slice record;
        std::string scratch;
        
        // 读取 MANIFEST 文件中的每一条 Record，
        // 把 Record 解码成一个 VersionEdit，
        // 然后调用 builder.Apply(&edit) 将 edit 应用到 builder 中。
        while (reader.ReadRecord(&record, &scratch) && s.ok()) {
            ++read_records;

            // 将 record 解码成 VersionEdit。
            VersionEdit edit;
            s = edit.DecodeFrom(record);
            if (s.ok()) {
                // 检查解码出来的 VersionEdit.comparator 与当前数据库的 comparator 是否一致。
                if (edit.has_comparator_ && edit.comparator_ != icmp_.user_comparator()->Name()) {
                    s = Status::InvalidArgument(
                        edit.comparator_ + " does not match existing comparator ",
                        icmp_.user_comparator()->Name());
                }
            }

            // 将 VersionEdit 应用到 builder 中。
            if (s.ok()) {
                builder.Apply(&edit);
            }

            // 记录最新状态下的 WAL 编号。
            if (edit.has_log_number_) {
                log_number = edit.log_number_;
                have_log_number = true;
            }

            // 记录最新状态下的 prev WAL 编号。
            if (edit.has_prev_log_number_) {
                prev_log_number = edit.prev_log_number_;
                have_prev_log_number = true;
            }

            // 记录最新状态下的 next_file 编号。
            if (edit.has_next_file_number_) {
                next_file = edit.next_file_number_;
                have_next_file = true;
            }

            // 记录最新状态下的 last_sequence 编号。
            if (edit.has_last_sequence_) {
                last_sequence = edit.last_sequence_;
                have_last_sequence = true;
            }
        }
    }
    // 读取 MANIFEST 文件完毕，关闭文件。
    delete file;
    file = nullptr;

    if (s.ok()) {
        if (!have_next_file) {
            s = Status::Corruption("no meta-nextfile entry in descriptor");
        } else if (!have_log_number) {
            s = Status::Corruption("no meta-lognumber entry in descriptor");
        } else if (!have_last_sequence) {
            s = Status::Corruption("no last-sequence-number entry in descriptor");
        }

        if (!have_prev_log_number) {
            prev_log_number = 0;
        }

        // 标记 next_file_number_ 和 log_number 都已经被占用了。
        MarkFileNumberUsed(prev_log_number);
        MarkFileNumberUsed(log_number);
    }

    if (s.ok()) {
        // 基于 MANIFEST 里的 VersionEdit列表，构造一个新的 Version。
        Version* v = new Version(this);
        builder.SaveTo(v);
        // Install recovered version
        // 把 New Version 作为当前 Version。
        Finalize(v);
        AppendVersion(v);
        manifest_file_number_ = next_file;
        next_file_number_ = next_file + 1;
        last_sequence_ = last_sequence;
        log_number_ = log_number;
        prev_log_number_ = prev_log_number;

        // See if we can reuse the existing MANIFEST file.
        // 检查是否可以继续使用当前的 MANIFEST 文件。
        if (ReuseManifest(dscname, current)) {
            // No need to save new manifest
        } else {
            // 如果不能继续使用的话，需要把当前的 MANIFEST 进行保存。
            *save_manifest = true;
        }
    } else {
        std::string error = s.ToString();
        Log(options_->info_log, "Error recovering version set with %d records: %s", read_records,
            error.c_str());
    }

    return s;
}

// 检查指定的 MANIFEST 是否还能继续使用。
bool VersionSet::ReuseManifest(const std::string& dscname, const std::string& dscbase) {
    if (!options_->reuse_logs) {
        return false;
    }
    FileType manifest_type;
    uint64_t manifest_number;
    uint64_t manifest_size;
    if (!ParseFileName(dscbase, &manifest_number, &manifest_type) ||
        manifest_type != kDescriptorFile || !env_->GetFileSize(dscname, &manifest_size).ok() ||
        // Make new compacted MANIFEST if old one is too big
        manifest_size >= TargetFileSize(options_)) {
        return false;
    }

    assert(descriptor_file_ == nullptr);
    assert(descriptor_log_ == nullptr);
    Status r = env_->NewAppendableFile(dscname, &descriptor_file_);
    if (!r.ok()) {
        Log(options_->info_log, "Reuse MANIFEST: %s\n", r.ToString().c_str());
        assert(descriptor_file_ == nullptr);
        return false;
    }

    Log(options_->info_log, "Reusing MANIFEST %s\n", dscname.c_str());
    descriptor_log_ = new log::Writer(descriptor_file_, manifest_size);
    manifest_file_number_ = manifest_number;
    return true;
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
    // 如果 number 小于 next_file_number_，那 number 肯定
    // 已经被分出去使用了，所以啥也不需要干。
    // 只有 number > = next_file_number_ 时，为了防止将来
    // NewFileNumber() 返回 number，需要把 next_file_number_
    // 拨到 number + 1，这样 NewFileNumber() 后面就永远不会
    // 返回 number。
    if (next_file_number_ <= number) {
        next_file_number_ = number + 1;
    }
}

/* 虽然函数名称叫做 Finalize，但实际上是在 pick 出下一次需要 Compaction 的 level
 */
void VersionSet::Finalize(Version* v) {
    // Precomputed best level for next compaction
    int best_level = -1;
    double best_score = -1;

    for (int level = 0; level < config::kNumLevels - 1; level++) {
        double score;
        if (level == 0) {
            // We treat level-0 specially by bounding the number of files
            // instead of number of bytes for two reasons:
            //
            // (1) With larger write-buffer sizes, it is nice not to do too
            // many level-0 compactions.
            //
            // (2) The files in level-0 are merged on every read and
            // therefore we wish to avoid too many files when the individual
            // file size is small (perhaps because of a small write-buffer
            // setting, or very high compression ratios, or lots of
            // overwrites/deletions).
            /* level 0 不看大小，只看 SSTable 的个数 */
            score = v->files_[level].size() / static_cast<double>(config::kL0_CompactionTrigger);
        } else {
            /* 获取当前 level SSTables 实际大小 */
            const uint64_t level_bytes = TotalFileSize(v->files_[level]);
            /* 计算 score 值，如果 level_bytes 超出阈值的话，那么 score 将大于 1
             */
            score = static_cast<double>(level_bytes) / MaxBytesForLevel(options_, level);
        }

        /* 注意这里是在 for 循环中进行的，也就是寻找所有 level 中，score
         * 最大的那个 level */
        if (score > best_score) {
            best_level = level;
            best_score = score;
        }
    }

    v->compaction_level_ = best_level;
    v->compaction_score_ = best_score;
}

Status VersionSet::WriteSnapshot(log::Writer* log) {
    // TODO: Break up into multiple records to reduce memory usage on recovery?

    // Save metadata
    VersionEdit edit;
    edit.SetComparatorName(icmp_.user_comparator()->Name());

    // Save compaction pointers
    for (int level = 0; level < config::kNumLevels; level++) {
        if (!compact_pointer_[level].empty()) {
            InternalKey key;
            key.DecodeFrom(compact_pointer_[level]);
            edit.SetCompactPointer(level, key);
        }
    }

    // Save files
    for (int level = 0; level < config::kNumLevels; level++) {
        const std::vector<FileMetaData*>& files = current_->files_[level];
        for (size_t i = 0; i < files.size(); i++) {
            const FileMetaData* f = files[i];
            edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
        }
    }

    std::string record;
    edit.EncodeTo(&record);
    return log->AddRecord(record);
}

int VersionSet::NumLevelFiles(int level) const {
    assert(level >= 0);
    assert(level < config::kNumLevels);
    return current_->files_[level].size();
}

const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
    // Update code if kNumLevels changes
    static_assert(config::kNumLevels == 7, "");
    std::snprintf(scratch->buffer, sizeof(scratch->buffer), "files[ %d %d %d %d %d %d %d ]",
                  int(current_->files_[0].size()), int(current_->files_[1].size()),
                  int(current_->files_[2].size()), int(current_->files_[3].size()),
                  int(current_->files_[4].size()), int(current_->files_[5].size()),
                  int(current_->files_[6].size()));
    return scratch->buffer;
}

uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
    uint64_t result = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
        const std::vector<FileMetaData*>& files = v->files_[level];
        for (size_t i = 0; i < files.size(); i++) {
            // 遍历版本 v 的每个 SST file

            if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
                // Entire file is before "ikey", so just add the file size
                // 
                // 当前 SST 文件所有的 key 都小于 ikey，所以直接把
                // 整个 SST 文件的大小加到 result 中。
                result += files[i]->file_size;
            } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
                // Entire file is after "ikey", so ignore
                // 
                // 当前 SST 文件所有的 key 都大于 ikey，忽略该文件。
                if (level > 0) {
                    // Files other than level 0 are sorted by meta->smallest, so
                    // no further files in this level will contain data for
                    // "ikey".
                    // 
                    // 如果当前在非 level-0 上，则该 level 上的 SST 是有序且不重合的，
                    // 碰到一个 SST 已经整体大于 ikey，那么后面的 SST 都不会包含 ikey，
                    // 可以不用往后看了。
                    break;
                }
            } else {
                // "ikey" falls in the range for this table.  Add the
                // approximate offset of "ikey" within the table.
                // 
                // 当前 SST 文件包含 ikey，计算 ikey 在 SST 文件中的大致偏移量。
                Table* tableptr;
                Iterator* iter = table_cache_->NewIterator(ReadOptions(), files[i]->number,
                                                           files[i]->file_size, &tableptr);
                if (tableptr != nullptr) {
                    result += tableptr->ApproximateOffsetOf(ikey.Encode());
                }
                delete iter;
            }
        }
    }
    return result;
}

// 遍历所有的 Version，将所有 level 上的 SST 文件编号加入到 live 中
void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
    for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
        for (int level = 0; level < config::kNumLevels; level++) {
            const std::vector<FileMetaData*>& files = v->files_[level];
            for (size_t i = 0; i < files.size(); i++) {
                live->insert(files[i]->number);
            }
        }
    }
}

int64_t VersionSet::NumLevelBytes(int level) const {
    assert(level >= 0);
    assert(level < config::kNumLevels);
    return TotalFileSize(current_->files_[level]);
}

int64_t VersionSet::MaxNextLevelOverlappingBytes() {
    int64_t result = 0;
    std::vector<FileMetaData*> overlaps;

    // 计算每个 level 上的每个 SST 文件与下一层 level 的 overlap 大小，
    // 返回最大的那个 overlap 大小。
    for (int level = 1; level < config::kNumLevels - 1; level++) {
        for (size_t i = 0; i < current_->files_[level].size(); i++) {
            const FileMetaData* f = current_->files_[level][i];
            current_->GetOverlappingInputs(level + 1, &f->smallest, &f->largest, &overlaps);
            const int64_t sum = TotalFileSize(overlaps);
            if (sum > result) {
                result = sum;
            }
        }
    }
    return result;
}

// Stores the minimal range that covers all entries in inputs in
// *smallest, *largest.
// REQUIRES: inputs is not empty
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                          InternalKey* largest) {
    assert(!inputs.empty());
    smallest->Clear();
    largest->Clear();
    for (size_t i = 0; i < inputs.size(); i++) {
        FileMetaData* f = inputs[i];
        if (i == 0) {
            *smallest = f->smallest;
            *largest = f->largest;
        } else {
            if (icmp_.Compare(f->smallest, *smallest) < 0) {
                *smallest = f->smallest;
            }
            if (icmp_.Compare(f->largest, *largest) > 0) {
                *largest = f->largest;
            }
        }
    }
}

// Stores the minimal range that covers all entries in inputs1 and inputs2
// in *smallest, *largest.
// REQUIRES: inputs is not empty
void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1,
                           const std::vector<FileMetaData*>& inputs2, InternalKey* smallest,
                           InternalKey* largest) {
    std::vector<FileMetaData*> all = inputs1;
    all.insert(all.end(), inputs2.begin(), inputs2.end());
    GetRange(all, smallest, largest);
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
    ReadOptions options;
    options.verify_checksums = options_->paranoid_checks;
    options.fill_cache = false;

    // Level-0 files have to be merged together.  For other levels,
    // we will make a concatenating iterator per level.
    // TODO(opt): use concatenating iterator for level-0 if there is no overlap
    //
    // 如果要进行 Compaction 的 level 是 0，那么需要把 level 0 中每个 SST 都
    // 要创建一个 Iterator，level + 1 层只需要创建一个 Concatenating Iterator。
    // 所以需要的 Iterator 数量为 c->inputs_[0].size() + 1。
    // 如果要进行 Compaction 的 level 不是 0，那么只需要 2 个 Concatenating
    // Iterator。
    const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
    Iterator** list = new Iterator*[space];
    int num = 0;
    for (int which = 0; which < 2; which++) {
        if (!c->inputs_[which].empty()) {
            if (c->level() + which == 0) {
                // 如果 Compaction level 是 0，并且 which 为 0。
                // 为每个 level 0 的 SST 创建一个 Iterator。
                const std::vector<FileMetaData*>& files = c->inputs_[which];
                for (size_t i = 0; i < files.size(); i++) {
                    list[num++] =
                        table_cache_->NewIterator(options, files[i]->number, files[i]->file_size);
                }
            } else {
                // Create concatenating iterator for the files from this level
                // 
                // 为当前 level 创建一个 Concatenating Iterator。
                list[num++] = NewTwoLevelIterator(
                    new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]), &GetFileIterator,
                    table_cache_, options);
            }
        }
    }
    assert(num <= space);
    // 把 list 里的 Iterator 合并成一个 MergingIterator。
    Iterator* result = NewMergingIterator(&icmp_, list, num);
    delete[] list;
    return result;
}

/* 选择最终要执行的 Compaction 类型: Size Compaction or Seek Compaction */
Compaction* VersionSet::PickCompaction() {
    Compaction* c;
    // 要进行 Compaction 的 level。
    int level;

    // We prefer compactions triggered by too much data in a level over
    // the compactions triggered by seeks.
    //
    // current_->compaction_score_ 为所有 level 中最大的 score。
    // score = 当前 level 所有 SST 总大小 / 此 level 阈值。
    // current_->compaction_score 的值是在调用`VersionSet::Finalize(Version* v)`
    // 的时候计算出来的。
    // current->file_to_compact_ 表示某个 SST 被读取的次数已经达到了阈值，需要进行
    // Compaction。
    // current->file_to_compact_ 的值是在`Version::UpdateStats`的时候更新的。
    const bool size_compaction = (current_->compaction_score_ >= 1);
    const bool seek_compaction = (current_->file_to_compact_ != nullptr);

    // 优先级: size_compaction > seek_compaction 。
    // 如果满足 size_compaction，那么优先执行 size_compaction。
    if (size_compaction) {
        // 获取需要进行 Compaction 的 level。
        level = current_->compaction_level_; 
        assert(level >= 0);
        assert(level + 1 < config::kNumLevels);

        // 创建一个 Compaction 对象 c。
        c = new Compaction(options_, level);

        // 从 files_[level] 里找到 compact_pointer_[level] 之后的第一个 SST，
        // 作为本次需要进行 Compaction 的 SST，放到 c->inputs_[0] 中。
        // compact_pointer_[level] 的含义是 level 层上一次参与 Compaction 
        // 的 SST 的最大 InternalKey。
        for (size_t i = 0; i < current_->files_[level].size(); i++) {

            FileMetaData* f = current_->files_[level][i];

            if (compact_pointer_[level].empty() ||
                icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
                c->inputs_[0].push_back(f);
                break;
            }
        }

        // 上面找不到 SST，表示 level 层上还没有 SST 参与过 Compaction，
        // 那本次就选择 level 层上的第一个 SST 参与 Compaction。
        if (c->inputs_[0].empty()) {
            // Wrap-around to the beginning of the key space
            c->inputs_[0].push_back(current_->files_[level][0]);
        }
    } else if (seek_compaction) {
        // 不满足 size_compaction，但满足 seek_compaction，那么执行 seek_compaction。

        // 获取需要进行 Compaction 的 level。
        level = current_->file_to_compact_level_;
        // 创建一个 Compaction 对象 c。
        c = new Compaction(options_, level);
        // 把要进行 Compaction 的 SST 放到 c->inputs_[0] 中。
        c->inputs_[0].push_back(current_->file_to_compact_);
    } else {
        // 不满足 size_compaction，也不满足 seek_compaction，
        // 所以不需要进行 Compaction。
        return nullptr;
    }

    // 把需要进行 Compaction 的 version 设置到 c->input_version_ 中。
    c->input_version_ = current_;
    // 把 c->input_version_ 的引用计数加 1，防止被销毁。
    c->input_version_->Ref();

    // 如果要进行 Compaction 的 level 是 0，那么需要把 level 0 中所有与
    // 本次要进行 Compaction 的 SST 有重叠的 SST 都放到 c->inputs_[0] 中，
    // 做为本次 Compaction 的输入。
    if (level == 0) {
        // 取得 inputs_[0] 中所有 SST 里的最小 InternalKey 和最大 InternalKey
        InternalKey smallest, largest;
        GetRange(c->inputs_[0], &smallest, &largest);
        // Note that the next call will discard the file we placed in
        // c->inputs_[0] earlier and replace it with an overlapping set
        // which will include the picked file.
        //
        // 在 level-0 层中查找哪些 SST 与 [smallest, largest] 有重叠，
        // 把它们都放到 c->inputs_[0] 中。
        current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
        assert(!c->inputs_[0].empty());
    }

    // c->inputs_[0] 表示 level K 将要进行 Compact 的 sst files，
    // c->inputs_[1] 表示 level K+1 将要进行 Compact 的 sst files。
    // SetupOtherInputs(c) 会根据 c->inputs_[0] 来确定 c->inputs_[1]。
    SetupOtherInputs(c);

    return c;
}

/* Finds the largest key in a vector of files. Returns true if files it not
 * empty. 获取某一个 level 中最大的 InternalKey */
bool FindLargestKey(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files,
                    InternalKey* largest_key) {
    if (files.empty()) {
        return false;
    }
    *largest_key = files[0]->largest;
    for (size_t i = 1; i < files.size(); ++i) {
        FileMetaData* f = files[i];
        if (icmp.Compare(f->largest, *largest_key) > 0) {
            *largest_key = f->largest;
        }
    }
    return true;
}

// Finds minimum file b2=(l2, u2) in level file for which l2 > u1 and
// user_key(l2) = user_key(u1)
FileMetaData* FindSmallestBoundaryFile(const InternalKeyComparator& icmp,
                                       const std::vector<FileMetaData*>& level_files,
                                       const InternalKey& largest_key) {
    const Comparator* user_cmp = icmp.user_comparator();
    FileMetaData* smallest_boundary_file = nullptr;
    for (size_t i = 0; i < level_files.size(); ++i) {
        FileMetaData* f = level_files[i];
        /* 最小 InternalKey 大于 largest key，并且最小 InternalKey 的 User Key
         * 等于 largest key 的 User Key */
        if (icmp.Compare(f->smallest, largest_key) > 0 &&
            user_cmp->Compare(f->smallest.user_key(), largest_key.user_key()) == 0) {
            if (smallest_boundary_file == nullptr ||
                icmp.Compare(f->smallest, smallest_boundary_file->smallest) < 0) {
                smallest_boundary_file = f;
            }
        }
    }
    return smallest_boundary_file;
}

// Extracts the largest file b1 from |compaction_files| and then searches for a
// b2 in |level_files| for which user_key(u1) = user_key(l2). If it finds such a
// file b2 (known as a boundary file) it adds it to |compaction_files| and then
// searches again using this new upper bound.
//
// If there are two blocks, b1=(l1, u1) and b2=(l2, u2) and
// user_key(u1) = user_key(l2), and if we compact b1 but not b2 then a
// subsequent get operation will yield an incorrect result because it will
// return the record from b2 in level i rather than from b1 because it searches
// level by level for records matching the supplied user key.
//
// parameters:
//   in     level_files:      List of files to search for boundary files.
//   in/out compaction_files: List of files to extend by adding boundary files.
//
// 假设有两个 SST 位于同一层。 SST-1 的范围是 [key1, key3], 
// SST-2 的范围是 [key3, key5]。那么如果 SST-1 在 compaction_files 里，
// 但是 SST-2 不在，就可能会有问题。因为如果 SST-1 里的 key3 是最新的，
// 只把 SST-1 进行 Compaction 下沉到 level+1，那么在查询 key3 的时候，
// 就会返回 SST-2 里的 key3，而不是 SST-1 里的 key3。
void AddBoundaryInputs(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>& level_files,
                       std::vector<FileMetaData*>* compaction_files) {
    InternalKey largest_key;

    // Quick return if compaction_files is empty.
    // 
    // 先找到 compaction_files 里的 largest_key.
    if (!FindLargestKey(icmp, *compaction_files, &largest_key)) {
        return;
    }

    bool continue_searching = true;
    while (continue_searching) {

        // 寻找一个 smallest_user_key == largest_key.user_key() 
        // 的后继 SST，
        FileMetaData* smallest_boundary_file =
            FindSmallestBoundaryFile(icmp, level_files, largest_key);

        // If a boundary file was found advance largest_key, otherwise we're
        // done.
        if (smallest_boundary_file != NULL) {
            // 找到这个后继 SST 了，把它加入到 compaction_files 中。
            // 并且把 largest_key 更新为这个后继 SST 的 largest_key，
            // 继续寻找下一个后继 SST。
            compaction_files->push_back(smallest_boundary_file);
            largest_key = smallest_boundary_file->largest;
        } else {
            // 找不到后继 SST，可以直接结束了。
            continue_searching = false;
        }
    }
}

// 根据 c->inputs_[0] 来确定 c->inputs_[1]。
// c->inputs_[0] 表示 level K 将要进行 Compact 的 sst files，
// c->inputs_[1] 表示 level K+1 将要进行 Compact 的 sst files。
void VersionSet::SetupOtherInputs(Compaction* c) {
    const int level = c->level();
    InternalKey smallest, largest;

    // 扩大 c->inputs_[0] 里的 SST 集合。
    // 规则如下：
    //     假设 c->inputs_[0] 里有两个 SST，分别是 SST-1 和 SST-2。
    //     如果 level 上存在一个 SST，它的 smallest_user_key 与
    //     c_inputs_[0] 的 largest_user_key 相等，那么把这个 SST
    //     称为 Boundary SST。
    //     不断的把 Boundary SST 加入到 c->inputs_[0] 中，再继续查找
    //     新的 Boundary SST，直到找不到为止。
    AddBoundaryInputs(icmp_, current_->files_[level], &c->inputs_[0]);
    // 获取 c->inputs_[0] 的 key 范围，也就是 level 层要进行 Compaction 的范围。
    GetRange(c->inputs_[0], &smallest, &largest);

    // 在 level+1 层查找与 c->inputs_[0] 有重叠的 SST，放到 c->inputs_[1] 中。
    current_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);
    // 以同样的方式，扩大 c->inputs_[1] 里的 SST 集合。
    AddBoundaryInputs(icmp_, current_->files_[level + 1], &c->inputs_[1]);

    // Get entire range covered by compaction
    //
    // 获取整体的 compaction 范围, [all_start, all_limit]
    InternalKey all_start, all_limit;
    GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

    // See if we can grow the number of inputs in "level" without
    // changing the number of "level+1" files we pick up.
    // 
    // 如果 level+1 层有需要 Compaction 的 SST，那么就尝试把 level 层的 
    // Compaction SST 扩大，但是不能导致 level+1 层的 Compaction SST 数量变多。
    if (!c->inputs_[1].empty()) {
        std::vector<FileMetaData*> expanded0;
        // 找出 level 层所有与 [all_start, all_limit] 有重叠的 SST，
        // 放到 expanded0 中。
        current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
        // 在 level 层寻找 expanded0 的 Boundary SST，放到 expanded0 中。
        AddBoundaryInputs(icmp_, current_->files_[level], &expanded0);

        // 计算 level 层的 Compaction SST 总大小.
        const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
        // 计算 level+1 层的 Compaction SST 总大小.
        const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
        // 计算 expanded0 的总大小。
        const int64_t expanded0_size = TotalFileSize(expanded0);

        // 如果 expanded0 的总大小比 c->inputs_[0] 的总大小大，并且
        // expanded0 和 c->inputs_[1] 的 SST 数量总和小于阈值，
        // 就可以考虑把 expanded0 作为 level 层的 Compaction SST。
        if (expanded0.size() > c->inputs_[0].size() &&
            inputs1_size + expanded0_size < ExpandedCompactionByteSizeLimit(options_)) {
            InternalKey new_start, new_limit;
            // 获取 expanded0 的 key 范围, [new_start, new_limit]。
            GetRange(expanded0, &new_start, &new_limit);
            // 根据 expanded0 的范围，获取 level+1 层的 Compaction SST，
            // 作为 expanded1。
            std::vector<FileMetaData*> expanded1;
            current_->GetOverlappingInputs(level + 1, &new_start, &new_limit, &expanded1);
            AddBoundaryInputs(icmp_, current_->files_[level + 1], &expanded1);

            // 如果 expanded1 里 SST 的数量和最开始算出来的 level+1 层的
            // Compaction SST 数量一样，那么就可以把 expanded0 和 expanded1
            // 分别作为 level 和 level+1 层的 Compaction SST。
            if (expanded1.size() == c->inputs_[1].size()) {
                Log(options_->info_log,
                    "Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld "
                    "bytes)\n",
                    level, int(c->inputs_[0].size()), int(c->inputs_[1].size()), long(inputs0_size),
                    long(inputs1_size), int(expanded0.size()), int(expanded1.size()),
                    long(expanded0_size), long(inputs1_size));
                smallest = new_start;
                largest = new_limit;
                c->inputs_[0] = expanded0;
                c->inputs_[1] = expanded1;
                GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
            }
        }
    }

    // Compute the set of grandparent files that overlap this compaction
    // (parent == level+1; grandparent == level+2)
    // 
    // 计算 level+2 层所有与 [all_start, all_limit] 有重叠的 SST，
    // 放到 c->grandparents_ 中。
    if (level + 2 < config::kNumLevels) {
        current_->GetOverlappingInputs(level + 2, &all_start, &all_limit, &c->grandparents_);
    }

    // Update the place where we will do the next compaction for this level.
    // We update this immediately instead of waiting for the VersionEdit
    // to be applied so that if the compaction fails, we will try a different
    // key range next time.
    // 
    // 设置 compact_pointer_[level]，表示 level 层下一次 Compaction 的起始位置。
    compact_pointer_[level] = largest.Encode().ToString();
    c->edit_.SetCompactPointer(level, largest);
}

Compaction* VersionSet::CompactRange(int level, const InternalKey* begin, const InternalKey* end) {
    
    // 获取 level 层所有与 [begin, end] 有重叠的 SST，放到 inputs 中。
    std::vector<FileMetaData*> inputs;
    current_->GetOverlappingInputs(level, begin, end, &inputs);
    if (inputs.empty()) {
        return nullptr;
    }

    // Avoid compacting too much in one shot in case the range is large.
    // But we cannot do this for level-0 since level-0 files can overlap
    // and we must not pick one file and drop another older file if the
    // two files overlap.
    // 
    // 对于非 level-0 层，需要检查下 inputs 里 SST 文件大小的总和。
    // 如果 inputs 里 SST 文件大小的总和超过了阈值，那么就需要把 inputs
    // 进行裁剪，只保留一部分 SST。
    if (level > 0) {
        const uint64_t limit = MaxFileSizeForLevel(options_, level);
        uint64_t total = 0;
        for (size_t i = 0; i < inputs.size(); i++) {
            uint64_t s = inputs[i]->file_size;
            total += s;
            if (total >= limit) {
                inputs.resize(i + 1);
                break;
            }
        }
    }

    // 把 inputs 里的 SST 放到 c->inputs_[0] 中。
    Compaction* c = new Compaction(options_, level);
    c->input_version_ = current_;
    c->input_version_->Ref();
    c->inputs_[0] = inputs;

    // 根据 c->inputs_[0] 来填充 c->inputs_[1]。
    SetupOtherInputs(c);
    return c;
}

Compaction::Compaction(const Options* options, int level)
    : level_(level),
      max_output_file_size_(MaxFileSizeForLevel(options, level)),
      input_version_(nullptr),
      grandparent_index_(0),
      seen_key_(false),
      overlapped_bytes_(0) {
    for (int i = 0; i < config::kNumLevels; i++) {
        level_ptrs_[i] = 0;
    }
}

Compaction::~Compaction() {
    if (input_version_ != nullptr) {
        input_version_->Unref();
    }
}

bool Compaction::IsTrivialMove() const {
    const VersionSet* vset = input_version_->vset_;
    // Avoid a move if there is lots of overlapping grandparent data.
    // Otherwise, the move could create a parent file that will require
    // a very expensive merge later on.
    return (num_input_files(0) == 1 && num_input_files(1) == 0 &&
            TotalFileSize(grandparents_) <= MaxGrandParentOverlapBytes(vset->options_));
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
    for (int which = 0; which < 2; which++) {
        for (size_t i = 0; i < inputs_[which].size(); i++) {
            edit->RemoveFile(level_ + which, inputs_[which][i]->number);
        }
    }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
    // Maybe use binary search to find right entry instead of linear search?
    const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
    for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
        const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
        while (level_ptrs_[lvl] < files.size()) {
            FileMetaData* f = files[level_ptrs_[lvl]];
            if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
                // We've advanced far enough
                if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
                    // Key falls in this file's range, so definitely not base
                    // level
                    return false;
                }
                break;
            }
            level_ptrs_[lvl]++;
        }
    }
    return true;
}

bool Compaction::ShouldStopBefore(const Slice& internal_key) {
    const VersionSet* vset = input_version_->vset_;
    // Scan to find earliest grandparent file that contains key.
    const InternalKeyComparator* icmp = &vset->icmp_;
    while (grandparent_index_ < grandparents_.size() &&
           icmp->Compare(internal_key, grandparents_[grandparent_index_]->largest.Encode()) > 0) {
        if (seen_key_) {
            overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
        }
        grandparent_index_++;
    }
    seen_key_ = true;

    if (overlapped_bytes_ > MaxGrandParentOverlapBytes(vset->options_)) {
        // Too much overlap for current output; start new output
        overlapped_bytes_ = 0;
        return true;
    } else {
        return false;
    }
}

void Compaction::ReleaseInputs() {
    if (input_version_ != nullptr) {
        input_version_->Unref();
        input_version_ = nullptr;
    }
}

}  // namespace leveldb
