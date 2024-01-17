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

##### VersionSet::Builder::Apply(VersionEdit* edit)

解析`edit`中的内容，每个 Level 都有哪些新增与移除的 SST 文件，然后更新`VersionSet::Builder::levels_`中的`deleted_files`和`added_files`，等到调用`VersionSet::Builder::SaveTo(Version* v)`时将`VersionSet::Builder::levels_`中的内容保存到版本`v`中。

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

##### VersionSet::Builder::SaveTo(Version* v)

将`VersionSet::Builder::levels_`中的内容保存到版本`v`中。

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

##### VersionSet::Builder::MaybeAddFile(Version* v, int level, FileMetaData* f)

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

### VersionSet::Recover(bool* save_manifest)

`VersionSet::Recover`会通过读取 CURRENT 文件，找到当前正在使用的 MANIFEST 文件，然后读取 MANIFEST 文件，将数据库恢复到上次关闭前的状态。

如果 MANIFEST 文件大小超过阈值，无法继续使用了，`save_manifest`会被设为`true`。表示当前的 MANIFEST 文件需要被保存。

```c++
// 从数据库中读取 CURRENT 文件，解析 MANIFEST文件，
// 恢复数据库的状态(每层 Level 都有哪些 SST 文件)。
Status VersionSet::Recover(bool* save_manifest) {
    struct LogReporter : public log::Reader::Reporter {
        Status* status;
        void Corruption(size_t bytes, const Status& s) override {
            if (this->status->ok()) *this->status = s;
        }
    };

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
        // 把 New Version 作为当前 Version。
        Finalize(v);
        AppendVersion(v);
        manifest_file_number_ = next_file;
        next_file_number_ = next_file + 1;
        last_sequence_ = last_sequence;
        log_number_ = log_number;
        prev_log_number_ = prev_log_number;

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
```

`edit.DecodeFrom(record)`的实现可参考[大白话解析LevelDB: VersionEdit](https://blog.csdn.net/sinat_38293503/article/details/135661654?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22135661654%22%2C%22source%22%3A%22sinat_38293503%22%7D#VersionEditDecodeFromconst_Slice_src_174)

### VersionSet::current()

`VersionSet::current()`的实现没啥好说的，直接返回当前正在使用的 Version，即`current_`。

```c++
Version* current() const { return current_; }
```

### VersionSet::ManifestFileNumber()

`VersionSet::ManifestFileNumber()`的实现没啥好说的，直接返回当前正在使用的 MANIFEST 文件编号，即`manifest_file_number_`。

```c++
uint64_t ManifestFileNumber() const { return manifest_file_number_; }
```

### VersionSet::NewFileNumber()

`VersionSet::NewFileNumber()`的实现没啥好说的，直接返回一个新的文件编号，即`next_file_number_`。

```c++
uint64_t NewFileNumber() { return next_file_number_++; }
```

### VersionSet::ReuseFileNumber(uint64_t file_number)

`VersionSet::ReuseFileNumber`方法用于在某些情况下回收 SST 文件编号。

这通常在创建新的SST文件但未实际使用它，然后决定删除它的情况下发生。

例如，当LevelDB进行压缩操作时，它会创建新的SST文件来存储压缩后的数据。然而，如果在创建新文件后，压缩操作由于某种原因（如错误或异常）被中断，那么新创建的SST文件可能就不再需要了。

在这种情况下，LevelDB可以通过调用`VersionSet::ReuseFileNumber`方法来回收该 SST 文件编号，而不是浪费一个新的编号。

```c++
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
```

### VersionSet::NumLevelFiles(int level)

`VersionSet::current_->files`中存储了每一层 SST 文件的 MetaData，只需要读取`current_->files_[level].size()`即可得到该层 SST 文件的数量。

```c++
int VersionSet::NumLevelFiles(int level) const {
    assert(level >= 0);
    assert(level < config::kNumLevels);
    return current_->files_[level].size();
}
```

### VersionSet::NumLevelBytes(int level)

通过调用`TotalFileSize(current_->files_[level])`来获取指定`level`上 SST 文件的总大小。

```c++
int64_t VersionSet::NumLevelBytes(int level) const {
    assert(level >= 0);
    assert(level < config::kNumLevels);
    return TotalFileSize(current_->files_[level]);
}
```

我们继续看`TotalFileSize`的实现:

很直观，就是遍历`files`，将每个 SST 文件的大小累加起来。

```c++
static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
    int64_t sum = 0;
    for (size_t i = 0; i < files.size(); i++) {
        sum += files[i]->file_size;
    }
    return sum;
}
```

### VersionSet::LastSequence()

`VersionSet::LastSequence()`的实现没啥好说的，直接返回当前数据库中最大的 Sequence Number，即`last_sequence_`。

```c++
uint64_t LastSequence() const { return last_sequence_; }
```

### VersionSet::SetLastSequence(uint64_t s)

`VersionSet::SetLastSequence`的实现没啥好说的，直接设置当前数据库中最大的 Sequence Number，即`last_sequence_`。

```c++
void SetLastSequence(uint64_t s) {
    assert(s >= last_sequence_);
    last_sequence_ = s;
}
```

### VersionSet::MarkFileNumberUsed(uint64_t number)

该函数的作用是标记某个文件编号`number`已经被使用。

如果`number`小于`next_file_number_`，那`number`肯定已经被分出去使用了，所以啥也不需要干。

只有`number > = next_file_number_` 时，为了防止将来`NewFileNumber()`返回`number`，需要把`next_file_number_`拨到`number + 1`，这样 `NewFileNumber()`后面就永远不会返回`number`。


```c++
void VersionSet::MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) {
        next_file_number_ = number + 1;
    }
}
```

### VersionSet::LogNumber()

返回当前正在使用的 WAL 编号。

实现没啥好讲的，直接返回`log_number_`。

```c++
uint64_t LogNumber() const { return log_number_; }
```

### VersionSet::PrevLogNumber()

和`VersionSet::LogNumber`不太一样，`VersionSet::PrevLogNumber`返回的是当前正在进行 Compaction 的 WAL 编号。

为什么说 PrevLogNumber 是当前正在进行 Compaction 的 WAL 编号呢？

当开始 Compaction 时，当前的 WAL 可能仍有一些正在写入的数据。为了确保这些数据不会丢失，LevelDB 并不会立即删除 WAL 文件。

相反，它开始写入一个新的 WAL 文件，并在`prev_log_number_`中保留对旧日志文件（正在被压缩的文件）的引用。

这样，即使在 Compaction 过程中发生崩溃，LevelDB 仍然可以从旧的日志文件中恢复数据。

一旦 Compaction 完成并且旧 WAL 文件中的所有数据都安全地写入到SST文件中，旧的 WAL 文件就会被删除，`prev_log_number_`被设置为零。

```c++
uint64_t PrevLogNumber() const { return prev_log_number_; }
```

### Compaction* VersionSet::PickCompaction()

返回一个`Compaction`对象来表示这次 Compaction 所需的信息，主要包含:

- 要对哪个 Level 进行 Compaction
- 参与 Compaction 的 SST 文件集合

```c++
Compaction* VersionSet::PickCompaction() {
    Compaction* c;
    // 要进行 Compaction 的 level。
    int level;

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
```

我们继续来看下`SetupOtherInputs(c)`的实现:

```c++
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

    // 获取整体的 compaction 范围, [all_start, all_limit]
    InternalKey all_start, all_limit;
    GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

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

    // 计算 level+2 层所有与 [all_start, all_limit] 有重叠的 SST，
    // 放到 c->grandparents_ 中。
    if (level + 2 < config::kNumLevels) {
        current_->GetOverlappingInputs(level + 2, &all_start, &all_limit, &c->grandparents_);
    }

    // 设置 compact_pointer_[level]，表示 level 层下一次 Compaction 的起始位置。
    compact_pointer_[level] = largest.Encode().ToString();
    c->edit_.SetCompactPointer(level, largest);
}
```

### Compaction* VersionSet::CompactRange(int level, const InternalKey* begin, const InternalKey* end)

指定 Level 和一个范围 [begin, end]，返回一个 Compaction 对象。

```c++
Compaction* VersionSet::CompactRange(int level, const InternalKey* begin, const InternalKey* end) {
    
    // 获取 level 层所有与 [begin, end] 有重叠的 SST，放到 inputs 中。
    std::vector<FileMetaData*> inputs;
    current_->GetOverlappingInputs(level, begin, end, &inputs);
    if (inputs.empty()) {
        return nullptr;
    }

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
```

### VersionSet::MaxNextLevelOverlappingBytes()

计算每个 level 上的每个 SST 文件与下一层 level 的 overlap bytes，返回最大的那个 overlap bytes。

```c++
int64_t VersionSet::MaxNextLevelOverlappingBytes() {
    int64_t result = 0;
    std::vector<FileMetaData*> overlaps;

    // 计算每个 level 上的每个 SST 文件与下一层 level 的 overlap bytes，
    // 返回最大的那个 overlap bytes。
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
```

### VersionSet::MakeInputIterator(Compaction* c)

读取`Compaction* c`里包含的输入文件，创建一个可以遍历这些文件的 Iterator。

```c++
Iterator* VersionSet::MakeInputIterator(Compaction* c) {
    ReadOptions options;
    options.verify_checksums = options_->paranoid_checks;
    options.fill_cache = false;

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
```

### VersionSet::NeedsCompaction()

判断当前是否需要进行 Compaction。

只需要看下`current_->compaction_score_`是否大于等于 1，或者`current_->file_to_compact_`是否不为空即可。

```c++
bool NeedsCompaction() const {
    Version* v = current_;
    return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);
}
```

### VersionSet::AddLiveFiles(std::set<uint64_t>* live)

遍历所有 Version，将每个 level 上的 SST 文件编号都加入到`live`中。

```c++
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
```

### VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& key)

计算`key`在指定版本`v`中的大致偏移量。

假设该版本的数据库状态如下:
```
          +----------------------------+
Level-0   |  SST0      SST1      SST2  |
          +----------------------------+
Level-1   |  SST3      SST4      SST5  |
          +----------------------------+
Level-2   |            ...             |
          +----------------------------+
Level-3   |            ...             |
          +----------------------------+
Level-4   |            ...             |
          +----------------------------+
```
假设目标`key`是 SST4 中的第一个，每个 SST 的大小为 4KB，则 offset 为$4 * 4KB = 16KB$。

```c++
uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
    uint64_t result = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
        const std::vector<FileMetaData*>& files = v->files_[level];
        for (size_t i = 0; i < files.size(); i++) {
            // 遍历版本 v 的每个 SST file

            if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
                // 当前 SST 文件所有的 key 都小于 ikey，所以直接把
                // 整个 SST 文件的大小加到 result 中。
                result += files[i]->file_size;
            } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
                // 当前 SST 文件所有的 key 都大于 ikey，忽略该文件。
                if (level > 0) {
                    // 如果当前在非 level-0 上，则该 level 上的 SST 是有序且不重合的，
                    // 碰到一个 SST 已经整体大于 ikey，那么后面的 SST 都不会包含 ikey，
                    // 可以不用往后看了。
                    break;
                }
            } else {
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
```