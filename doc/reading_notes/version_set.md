# VersionSet

在 LevelDB 中，`VersionSet` 类是一个关键的内部组件，负责管理数据库的不同版本。这个类跟踪了所有的 SSTables（排序字符串表）和它们在数据库中的布局。每次对数据库进行修改时（如添加、删除数据），LevelDB 会创建一个新的 `Version` 对象，这个对象由 `VersionSet` 管理。`VersionSet` 通过维护这些 `Version` 对象，可以快速地切换到数据库的不同历史状态，从而支持 LevelDB 的快照和压缩操作。简而言之，`VersionSet` 在 LevelDB 中扮演着版本控制和数据组织的角色，确保了数据库操作的高效和数据的一致性。


## VersionSet 接口概览

```c++
class VersionSet {
   public:
    VersionSet(const std::string& dbname, const Options* options, TableCache* table_cache,
               const InternalKeyComparator*);
    VersionSet(const VersionSet&) = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    ~VersionSet();

    // 将 VersionEdit 应用到当前 Version，生成新的 Version，
    // 并将新 Version 加入到 VersionSet 中。
    // Version N + VersionEdit = Version N+1。
    Status LogAndApply(VersionEdit* edit, port::Mutex* mu) EXCLUSIVE_LOCKS_REQUIRED(mu);

    // 从manifest文件中恢复数据库的状态。
    // 在LevelDB启动时，会调用这个方法来加载数据库的当前状态。
    Status Recover(bool* save_manifest);

    // 返回当前的 Version。
    Version* current() const { return current_; }

    // 返回当前正在使用的 MANIFEST 文件编号。
    uint64_t ManifestFileNumber() const { return manifest_file_number_; }

    // 生成一个新的文件编号。
    uint64_t NewFileNumber() { return next_file_number_++; }

    // VersionSet::ReuseFileNumber方法用于在某些情况下重复使用SST文件编号。
    // 这通常在创建新的SST文件但未实际使用它，然后决定删除它的情况下发生。
    // 例如，当LevelDB进行压缩操作时，它会创建新的SST文件来存储压缩后的数据。
    // 然而，如果在创建新文件后，压缩操作由于某种原因（如错误或异常）被中断，
    // 那么新创建的SST文件可能就不再需要了。在这种情况下，LevelDB可以通过
    // 调用VersionSet::ReuseFileNumber方法来重复使用该SST文件的编号，
    // 而不是浪费一个新的编号。
    void ReuseFileNumber(uint64_t file_number) {
        if (next_file_number_ == file_number + 1) {
            next_file_number_ = file_number;
        }
    }

    // 返回某一层上的 SST 文件数量。
    int NumLevelFiles(int level) const;

    // 返回某一层上 SST 文件的总大小。
    int64_t NumLevelBytes(int level) const;

    // 返回当前数据库中最大的 Sequence Number。
    uint64_t LastSequence() const { return last_sequence_; }

    // 设置当前数据库中最大的 Sequence Number。
    void SetLastSequence(uint64_t s) {
        assert(s >= last_sequence_);
        last_sequence_ = s;
    }

    // 标记某个文件编号已经被使用。
    void MarkFileNumberUsed(uint64_t number);

    // 返回当前正在使用的 WAL 文件编号。
    uint64_t LogNumber() const { return log_number_; }

    // prev_log_number_ 记录了当前正在进行 Compaction 的 WAL 文件编号。
    // 当开始 Compaction 时，当前的 WAL 可能仍有一些正在写入的数据。为了
    // 确保这些数据不会丢失，LevelDB 并不会立即删除 WAL 文件。相反，它开始写入
    // 一个新的 WAL 文件，并在prev_log_number_中保留对旧日志文件（正在被压缩的文件）的引用。
    // 这样，即使在 Compaction 过程中发生崩溃，LevelDB 仍然可以从旧的日志文件中恢复数据。
    // 一旦 Compaction 完成并且旧 WAL 文件中的所有数据都安全地写入到SST文件中，
    // 旧的 WAL 文件就会被删除，prev_log_number_被设置为零。
    uint64_t PrevLogNumber() const { return prev_log_number_; }

    // 选择一个合适的 Level 和 SST 文件集合进行 Compaction，
    // 用 Compaction 对象表示这次 Compaction 所需的信息。
    Compaction* PickCompaction();

    // 指定 Level 和一个范围 [begin, end]，返回一个 Compaction 对象，
    Compaction* CompactRange(int level, const InternalKey* begin, const InternalKey* end);

    // 对每一层计算 level-i 与 level-i+1 之间的 overlap bytes。
    // 返回最大的 overlap bytes。
    int64_t MaxNextLevelOverlappingBytes();

    // 读取 Compaction 里包含的输入文件，创建一个可以遍历这些文件的 Iterator。
    Iterator* MakeInputIterator(Compaction* c);

    // 判断当前是否需要进行 Compaction。
    bool NeedsCompaction() const {
        Version* v = current_;
        return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);
    }

    // 将所有活跃的 SST 文件编号添加到 live 中。
    // 活跃的 SST 指正在参与 compaction 或者未过期的 SST。
    // 有些 SST 由于创建了快照，compaction 时没有将其删除。
    // 快照释放后，这些 SST 就过期了，不属于任何一个 level，但是仍然存在于磁盘上。
    void AddLiveFiles(std::set<uint64_t>* live);

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

    struct LevelSummaryStorage {
        char buffer[100];
    };
    // 以可视化的方式打印每个 level 的 SST 数量。
    // 比如 scratch->buffer = "files[ 1, 5, 7, 9, 0, 0, 0]"
    // 表示 Level-0 有 1 个 SST，Level-1 有 5 个 SST，以此类推。
    const char* LevelSummary(LevelSummaryStorage* scratch) const;
};
```

## VersionSet 中各个接口的实现

### VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu)

```c++
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

    // 每产生一个新的 Version，都需要创建一个对应的 New MANIFEST。
    std::string new_manifest_file;
    Status s;
    // 当数据库是第一次被打开时，descriptor_log_ 为 nullptr。
    if (descriptor_log_ == nullptr) {

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

    {
        // 只有以下 3 种情况会读取 MANIFEST 和 CURRETN 文件:
        //   - 数据库启动时
        //   - Recover 时
        //   - Compaction 后调用 VersionSet::LogAndApply
        // 此处可以 unlock mu，是因为上面这 3 种情况不会有 2 种同时发生。
        mu->Unlock();

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

        // 如果 MANIFEST 是新创建的，还需要更新 CURRENT。
        if (s.ok() && !new_manifest_file.empty()) {
            s = SetCurrentFile(env_, dbname_, manifest_file_number_);
        }

        mu->Lock();
    }

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
```

#### VersionSet::Builder 的实现

`VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu)`中通过 Builder 将`edit`应用到当前版本`current_`，生成新的 Version。

```c++
Version* v = new Version(this);
{
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
}
Finalize(v);
```

我们来看下 Builder 是如何根据`edit`与`current_`生成新 Version 的。

##### VersionSet::Builder 的构造

```c++
class VersionSet::Builder {
   private:
    // 排序函数，用于比较两个 FileMetaData 的大小。
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

    // 一个基于 BySmallestKey 比较函数的集合。
    typedef std::set<FileMetaData*, BySmallestKey> FileSet;

    struct LevelState {
        std::set<uint64_t> deleted_files;
        FileSet* added_files;
    };

    // vset_ 和 base_ 均由构造函数初始化
    VersionSet* vset_;
    Version* base_;

    // levels_ 中记录了各个 level 中新增与移除了哪些 SST 文件
    LevelState levels_[config::kNumLevels];

   public:
    // 构造函数中主要是初始化各个 level 的 added_files 集合。
    Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
        base_->Ref();
        BySmallestKey cmp;
        cmp.internal_comparator = &vset_->icmp_;

        /* 为每一层 level 初始化 FileSet */
        for (int level = 0; level < config::kNumLevels; level++) {
            levels_[level].added_files = new FileSet(cmp);
        }
    }
};
```

##### VersionSet::Apply(VersionEdit* edit)

解析`edit`中的内容，每个 Level 都有哪些新增与移除的 SST 文件，然后更新`VersionSet::levels_`中的`deleted_files`和`added_files`，等到调用`VersionSet::SaveTo(Version* v)`时将`VersionSet::levels_`中的内容保存到版本`v`中。

```c++
void Apply(VersionEdit* edit) {
    // vset_->compact_pointer_[i] 记录 level-i 下一次 Compaction 的起始 InternalKey。
    // 下面这段 for 循环只是把 VersionEdit::compact_pointers_ 中的内容转换成 VersionSet::compact_pointer_。
    for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
        const int level = edit->compact_pointers_[i].first;
        // vset_->compact_pointer_[i] 记录 level-i 下一次 Compaction 的起始 InternalKey
        vset_->compact_pointer_[level] = edit->compact_pointers_[i].second.Encode().ToString();
    }

    // 将 deleted files 记录到 levels_ 中。
    for (const auto& deleted_file_set_kvp : edit->deleted_files_) {
        const int level = deleted_file_set_kvp.first;
        const uint64_t number = deleted_file_set_kvp.second;
        levels_[level].deleted_files.insert(number);
    }

    // 将 new files 记录到 levels_ 中。
    for (size_t i = 0; i < edit->new_files_.size(); i++) {
        const int level = edit->new_files_[i].first;
        FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
        f->refs = 1;

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
```

##### VersionSet::SaveTo(Version* v)

将`VersionSet::levels_`中的内容保存到版本`v`中。

```c++
void SaveTo(Version* v) {
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
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
            for (std::vector<FileMetaData*>::const_iterator bpos =
                        std::upper_bound(base_iter, base_end, added_file, cmp);
                    base_iter != bpos; ++base_iter) {
                MaybeAddFile(v, level, *base_iter);
            }

            MaybeAddFile(v, level, added_file);
        }

        for (; base_iter != base_end; ++base_iter) {
            MaybeAddFile(v, level, *base_iter);
        }

// 如果是 DEBUG 模式
#ifndef NDEBUG
        if (level > 0) {
            // 如果 level > 0，则检查该层上的 SST 是否有 overlap。
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
```

##### VersionSet::MaybeAddFile(Version* v, int level, FileMetaData* f)

如果 f 出现在 deleted_files 中，说明该 SST 文件已经被删除了，不需要添加到 v->files_[level] 中。

否则的话，将 f 添加到 v->files_[level] 中。

```c++
void MaybeAddFile(Version* v, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->number) > 0) {
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
```