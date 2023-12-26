# Version

- [Version](#version)
  - [Version 的职责](#version-的职责)
  - [Version 接口概览](#version-接口概览)
  - [Version 中各个接口的实现](#version-中各个接口的实现)
    - [Version::Get(const ReadOptions\&, const LookupKey\& key, std::string\* val, GetStats\* stats)](#versiongetconst-readoptions-const-lookupkey-key-stdstring-val-getstats-stats)
    - [Version::AddIterators(const ReadOptions\&, std::vector\<Iterator\*\>\* iters)](#versionadditeratorsconst-readoptions-stdvectoriterator-iters)
    - [Version::UpdateStats(const GetStats\& stats)](#versionupdatestatsconst-getstats-stats)
    - [Version::RecordReadSample(Slice key)](#versionrecordreadsampleslice-key)
    - [Version::GetOverlappingInputs(int level, const InternalKey\* begin, const InternalKey\* end, std::vector\<FileMetaData\*\>\* inputs)](#versiongetoverlappinginputsint-level-const-internalkey-begin-const-internalkey-end-stdvectorfilemetadata-inputs)
    - [Version::OverlapInLevel(int level, const Slice\* smallest\_user\_key, const Slice\* largest\_user\_key)](#versionoverlapinlevelint-level-const-slice-smallest_user_key-const-slice-largest_user_key)
    - [Version::PickLevelForMemTableOutput(const Slice\& smallest\_user\_key, const Slice\& largest\_user\_key)](#versionpicklevelformemtableoutputconst-slice-smallest_user_key-const-slice-largest_user_key)
    - [Version::DebugString()](#versiondebugstring)
    - [Version::ForEachOverlapping(Slice user\_key, Slice internal\_key, void\* arg, bool (\*func)(void\*, int, FileMetaData\*))](#versionforeachoverlappingslice-user_key-slice-internal_key-void-arg-bool-funcvoid-int-filemetadata)


## Version 的职责

在 LevelDB 中，Version 扮演着核心的角色，负责管理和维护数据库的不同版本。LevelDB 是一种基于 LSM-tree（Log-Structured Merge-tree）的键值存储，它通过创建数据的不同“版本”来优化读写操作。以下是 Version 的主要职责：

- **管理 SSTable（Sorted String Table）文件：** `Version` 负责维护和跟踪数据库中所有层级（Level）上的 SSTable 文件。这包括文件的添加、删除以及在这些层级之间的迁移。

- **维护键值对的元数据：** `Version` 存储了 SSTable 文件中键值对的元数据，例如键的范围、文件大小等信息。这有助于在查询操作中快速定位需要读取的 SSTable 文件。

- **查询操作：** 当执行读取操作时，`Version` 负责确定哪些 SSTable 文件可能包含给定的键，并在这些文件中进行查找。这是通过检查每个文件的键范围来实现的。

- **合并和压缩操作：** LevelDB 定期执行合并和压缩操作以优化存储空间和提高读取性能。`Version` 负责协调这些操作，确定哪些 SSTable 文件应该合并或移动到不同的层级。

- **版本控制和垃圾收集：** `Version` 管理数据库的不同版本，确保读取操作可以访问一致的数据快照。它还负责识别不再需要的旧数据文件并将其标记为垃圾收集。

- **读写隔离和一致性保证：** 通过管理不同版本的数据，`Version` 为数据库操作提供了必要的读写隔离和一致性保证，确保数据的正确性和一致性。

- **统计和性能监控：** `Version` 可能还涉及到收集和维护性能统计信息，如读取/写入次数、合并操作次数等，以帮助优化数据库性能。

## Version 接口概览

```c++
class Version {
   public:
    // GetStats 用于记录 Get 操作的一些额外返回信息:
    //   - key 所在 SST 的 MetaData
    //   - key 所在 SST 的 level
    struct GetStats {
        FileMetaData* seek_file;
        int seek_file_level;
    };

    // 在指定版本中，查找一个给定 key 的 value。
    // 如果该 key 存在，则返回 OK，并且在 stats 中记录一些查找信息，比如
    // key 所在的文件、level 等。
    Status Get(const ReadOptions&, const LookupKey& key, std::string* val, GetStats* stats);

    // 创建一个 iters，可以用来遍历当前版本的所有 key-value。
    void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

    // 该方法通常在读操作后被调用，stats 中包含了 Get 操作所访问的 
    // SST 文件的 MetaData，MetaData 中有个成员 allowed_seeks, 表示该 SST
    // 允许被访问的次数。每调用一次 UpdateStats(stats)，会将 allowed_seeks 减一，
    // 当 allowed_seeks 为 0 时，会返回 true，
    // 表示该 SST 的访问频率过高，需要进行 Compaction。
    bool UpdateStats(const GetStats& stats);

    // 通过 Iterator 每读取 config::kReadBytesPeriod 个字节，
    // 就会对当前 key 调用一次 RecordReadSample()，
    // 记录 key 的访问频率。
    // RecordReadSample(key) 会遍历所有与 key 有 overlap 的 SST，
    // 所以重叠的 SST 不止一个，就对第一个 SST 调用一次 UpdateStats。
    bool RecordReadSample(Slice key);

    // 给定一个范围 [begin, end]，返回在 level 层所有与之有 overlap 的 SST 文件，
    // 记录到 inputs 中。
    void GetOverlappingInputs(int level,
                              const InternalKey* begin,  // nullptr means before all keys
                              const InternalKey* end,    // nullptr means after all keys
                              std::vector<FileMetaData*>* inputs);

    // 检查 level 层是否有 SST 与 [smallest_user_key, largest_user_key] 有 overlap
    bool OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key);

    // 给定 [smallest_user_key, largest_user_key] 代表一个 MemTable， 
    // 返回一个 level，用于将该 MemTable 落盘为 SST。
    int PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key);

    // 获取指定 level 上的 SST 数量。
    int NumFiles(int level) const { return files_[level].size(); }

    // 用可视化的方式打印 Version 的内容，供调试。
    std::string DebugString() const;
};
```

## Version 中各个接口的实现

### Version::Get(const ReadOptions&, const LookupKey& key, std::string* val, GetStats* stats)

```c++
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
```

`Version::Get`中用到的`ForEachOverlapping`属于迭代器设计模式，其具体实现可移步参考[Version::ForEachOverlapping 的实现](#versionforeachoverlappingslice-user_key-slice-internal_key-void-arg-bool-funcvoid-int-filemetadata)

### Version::AddIterators(const ReadOptions&, std::vector<Iterator*>* iters)

```c++
void Version::AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) {
    // 对于 level-0 的 SST，会有 SST 重叠的情况，所以需要每个 SST 的 iterator。
    for (size_t i = 0; i < files_[0].size(); i++) {
        iters->push_back(vset_->table_cache_->NewIterator(options, files_[0][i]->number,
                                                          files_[0][i]->file_size));
    }

    // 对于 level > 0 的 SST，不会有 SST 重叠的情况，所以可以使用一个 ConcatenatingIterator，
    // 就是将一个 level 上的所有 SST 的 iterator 拼接起来，作为一个 iterator 使用。
    for (int level = 1; level < config::kNumLevels; level++) {
        if (!files_[level].empty()) {
            iters->push_back(NewConcatenatingIterator(options, level));
        }
    }
}
```

### Version::UpdateStats(const GetStats& stats)

```c++
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
```

### Version::RecordReadSample(Slice key)

```c++
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

    // 如果至少有 2 个 SST 包含了 user_key，调用 UpdateStats 更新一下
    // 第一个 SST 的 allowed_seeks。
    if (state.matches >= 2) {
        return UpdateStats(state.stats);
    }
    return false;
}
```

### Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inputs)

`Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inputs)` 用于检测 level 层中的哪些 SST 文件与 [begin,end] 有重叠，将他们存储在 inputs 中返回。

比如 [begin,end] 为 [10, 15]，而 level 层中某个 SST 文件的范围为 [5, 20]，那么这个 SST 文件就与 [begin,end] 有重叠，需要将其加入`inputs`中。

值得注意的是，对于 level-0 层的 SST 文件，可能会互相重叠，所以需要检查下是否存在间接重叠的情况: 

[begin,end] 与 files_[0][i] 直接重叠，files[0][i] 和 files_[0][i-1] 直接重叠，导致 [begin,end] 与 files_[0][i-1] 虽然不直接重叠，但是间接重叠。

简介重叠也算重叠，files_[0][i-1] 也需要加入到`inputs`中。

```c++
// 在 level 层中查找有哪些 SST 文件与 [begin,end] 有重叠，将他们存储在 inputs 中返回。
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
            // 当前 SST 的 largest_user_key 比 user_begin 还要小，可以跳过当前 SST 了
        } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
            // 当前 SST 的 smallest_user_key 比 user_end 还要大，可以跳过当前 SST 了
        } else {
            // 当前 SST 和 [user_begin, user_end] 有交集，将其加入 inputs 中
            inputs->push_back(f);

            
            if (level == 0) {
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
```

### Version::OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key)

```c++
bool Version::OverlapInLevel(int level, const Slice* smallest_user_key,
                             const Slice* largest_user_key) {
    return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level], smallest_user_key, largest_user_key);
}
```

`OverlapInLevel` 的逻辑甩给了 `SomeFileOverlapsRange`，那我们得继续看`SomeFileOverlapsRange`的实现。

```c++
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp, bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files, const Slice* smallest_user_key,
                           const Slice* largest_user_key) {
    // 通过比较 User Key 来判断是否存在 overlap，
    // 所以要用到 User Comparator。
    const Comparator* ucmp = icmp.user_comparator();

    // 对于 level-0 的 sst files，需要将输入 files 挨个遍历一遍，
    // 看下是否存在 overlap。
    if (!disjoint_sorted_files) {
        
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

    // index 表示可能与 [smallest_user_key, largest_user_key] 有 overlap 的 files[index]
    uint32_t index = 0;
    // 如果 smallest_user_key 为 nullptr，表示 smallest_user_key 为无限小，
    // 那唯一有可能与 [smallest_user_key, largest_user_key] 有 overlap 的 file 为 files[0]。
    if (smallest_user_key != nullptr) {
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
```

现在我们明白了`Version::OverlapInLevel`的逻辑了，对`FindFile(icmp, files, small_key.Encode())`的实现感兴趣的同学，可以继续看下`FindFile`是如何以**二分查找**的形式找到第一个**可能**与`[smallest_user_key, largest_user_key]`有重叠的`SST`文件的。


```c++
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
```

### Version::PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key)

`Version::PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key)` 负责挑选合适的 level-i 用于放置新的`SST`。

其中， `smallest_user_key` 是 New SST 里最小的 User Key， `largest_user_key` 是 New SST 里最大的 User Key。

它首先检查 New SST 的 key 范围是否与 Level-0 层有重叠，如果没有，就尝试将 New SST 放置在更高的层级，直到遇到与 New SST 的 key 范围有重叠的层级，或者 New SST 的 key 范围与下两层的 SST 文件重叠的总大小超过阈值。

```c++
int Version::PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                        const Slice& largest_user_key) {
    int level = 0;

    // 如果 New SST 和 level 0 层有重叠的话，那只能选择 level 0 层。
    // 否则的话，就继续往更大的 level 找，直到找到第一个和 New SST 有重叠的 level。
    if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
        
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
                // 检查 New SST 与 level+2 层的哪些 SST 有重叠，
                // 这些重叠的 SST 存储在 std::vector<FileMetaData*> overlaps 中。
                GetOverlappingInputs(level + 2, &start, &limit, &overlaps);

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
```

### Version::DebugString()

返回的 DebugString 格式如下:

```
--- level 1 ---
{文件编号}:{文件大小}[smallest_key .. largest_key]
...
{文件编号}:{文件大小}[smallest_key .. largest_key]
--- level 2 ---
{文件编号}:{文件大小}[smallest_key .. largest_key]
...
{文件编号}:{文件大小}[smallest_key .. largest_key]
--- level N ---
{文件编号}:{文件大小}[smallest_key .. largest_key]
...
{文件编号}:{文件大小}[smallest_key .. largest_key]
```

```c++
std::string Version::DebugString() const {
    std::string r;
    for (int level = 0; level < config::kNumLevels; level++) {
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
```

### Version::ForEachOverlapping(Slice user_key, Slice internal_key, void* arg, bool (\*func)(void\*, int, FileMetaData\*))

`Version::ForEachOverlapping`属于迭代器设计模式，这种模式提供了一种方法来顺序访问一个聚合对象中各个元素，而又不暴露该对象的内部表示。

`Version::ForEachOverlapping`方法提供了一种遍历所有与给定 key 重叠的 SST 的方式。这个方法接收一个函数（或者函数对象），并将每个重叠的 SST 作为参数传递给这个函数。这样，调用者就可以对每个重叠的 SST 进行处理，而不需要关心如何按顺序取出这些 SST 文件。


```c++
void Version::ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                                 bool (*func)(void*, int, FileMetaData*)) {
    const Comparator* ucmp = vset_->icmp_.user_comparator();

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

    // 到其他 level 中寻找与 user_key 有重叠的 SST 文件，调用 func(arg, level, f)。
    for (int level = 1; level < config::kNumLevels; level++) {
        size_t num_files = files_[level].size();
        if (num_files == 0) continue;

        // 用二分查找的方式，在该 level 中找到第一个 largest_key >= internal_key 的 SST 文件 f。
        uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
        if (index < num_files) {
            FileMetaData* f = files_[level][index];
            if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
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
```
