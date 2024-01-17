# VersionEdit

LevelDB 在进行 Compaction 的过程中，会增加一些 SST 并且删除一些 SST，这些操作都会引起数据库状态的变化。

每个数据库状态都对应一个 Version 版本，Version 里对应的数据库状态，也就是每层 level 里都有哪些 SST 文件。

VersionEdit 就是用来记录两个 Version 版本之间的变化的。`Version N + VersionEdit = Version N+1`。

```c++
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

    std::string comparator_;       // Comparator 名称 
    uint64_t log_number_;          // 当前对应的 WAL 编号 
    uint64_t prev_log_number_;     // 前一个 WAL 编号 
    uint64_t next_file_number_;    // 下一个文件编号 
    SequenceNumber last_sequence_; // 最大序列号 

    bool has_comparator_; // 上面 5 个变量的 Exist 标志位
    bool has_log_number_;
    bool has_prev_log_number_;
    bool has_next_file_number_;
    bool has_last_sequence_;

    // 记录某一层下一次进行 Compaction 的起始 InternalKey 
    std::vector<std::pair<int, InternalKey>> compact_pointers_;

    DeletedFileSet deleted_files_; // 记录哪些文件被删除了 

    // 记录哪一层新增了哪些 SST 文件，使用 FileMetaData 来表示一个 SST
    std::vector<std::pair<int, FileMetaData>> new_files_;
};
```

## VersionEdit::EncodeTo(std::string* dst)

将 VersionEdit 序列化成 string，然后写入到 MANIFEST 文件中。

```c++
void VersionEdit::EncodeTo(std::string* dst) const {
    // 将 Comparator Name 压入 dst
    if (has_comparator_) {
        PutVarint32(dst, kComparator);
        PutLengthPrefixedSlice(dst, comparator_);
    }

    // 将 WAL 编号压入 dst
    if (has_log_number_) {
        PutVarint32(dst, kLogNumber);
        PutVarint64(dst, log_number_);
    }

    // 将 Prev WAL 编号压入 dst
    if (has_prev_log_number_) {
        PutVarint32(dst, kPrevLogNumber);
        PutVarint64(dst, prev_log_number_);
    }

    // 将下一个文件编号写入 dst
    if (has_next_file_number_) {
        PutVarint32(dst, kNextFileNumber);
        PutVarint64(dst, next_file_number_);
    }

    // 将最大的 SequenceNumber 写入 dst
    if (has_last_sequence_) {
        PutVarint32(dst, kLastSequence);
        PutVarint64(dst, last_sequence_);
    }

    // 将每层的 CompactPointer 写入 dst
    for (size_t i = 0; i < compact_pointers_.size(); i++) {
        PutVarint32(dst, kCompactPointer);
        PutVarint32(dst, compact_pointers_[i].first);  // level
        PutLengthPrefixedSlice(dst, compact_pointers_[i].second.Encode());
    }

    // 将每层被删除的 SST 编号写入 dst
    for (const auto& deleted_file_kvp : deleted_files_) {
        PutVarint32(dst, kDeletedFile);
        PutVarint32(dst, deleted_file_kvp.first);   // level
        PutVarint64(dst, deleted_file_kvp.second);  // file number
    }

    // 将每层新增的 SST MetaData 写入 dst
    for (size_t i = 0; i < new_files_.size(); i++) {
        const FileMetaData& f = new_files_[i].second;
        PutVarint32(dst, kNewFile);
        PutVarint32(dst, new_files_[i].first);  // level
        PutVarint64(dst, f.number);
        PutVarint64(dst, f.file_size);
        PutLengthPrefixedSlice(dst, f.smallest.Encode());
        PutLengthPrefixedSlice(dst, f.largest.Encode());
    }
}
```

## VersionEdit::DecodeFrom(const Slice& src)

没啥好说的，按照`VersionEdit::EncodeTo()`的逆过程来做就行了。

```c++
Status VersionEdit::DecodeFrom(const Slice& src) {
    Clear();
    Slice input = src;
    const char* msg = nullptr;
    uint32_t tag;

    // Temporary storage for parsing
    int level;
    uint64_t number;
    FileMetaData f;
    Slice str;
    InternalKey key;

    while (msg == nullptr && GetVarint32(&input, &tag)) {
        switch (tag) {
            case kComparator:
                if (GetLengthPrefixedSlice(&input, &str)) {
                    comparator_ = str.ToString();
                    has_comparator_ = true;
                } else {
                    msg = "comparator name";
                }
                break;

            case kLogNumber:
                if (GetVarint64(&input, &log_number_)) {
                    has_log_number_ = true;
                } else {
                    msg = "log number";
                }
                break;

            case kPrevLogNumber:
                if (GetVarint64(&input, &prev_log_number_)) {
                    has_prev_log_number_ = true;
                } else {
                    msg = "previous log number";
                }
                break;

            case kNextFileNumber:
                if (GetVarint64(&input, &next_file_number_)) {
                    has_next_file_number_ = true;
                } else {
                    msg = "next file number";
                }
                break;

            case kLastSequence:
                if (GetVarint64(&input, &last_sequence_)) {
                    has_last_sequence_ = true;
                } else {
                    msg = "last sequence number";
                }
                break;

            case kCompactPointer:
                if (GetLevel(&input, &level) && GetInternalKey(&input, &key)) {
                    compact_pointers_.push_back(std::make_pair(level, key));
                } else {
                    msg = "compaction pointer";
                }
                break;

            case kDeletedFile:
                if (GetLevel(&input, &level) && GetVarint64(&input, &number)) {
                    deleted_files_.insert(std::make_pair(level, number));
                } else {
                    msg = "deleted file";
                }
                break;

            case kNewFile:
                if (GetLevel(&input, &level) && GetVarint64(&input, &f.number) &&
                    GetVarint64(&input, &f.file_size) && GetInternalKey(&input, &f.smallest) &&
                    GetInternalKey(&input, &f.largest)) {
                    new_files_.push_back(std::make_pair(level, f));
                } else {
                    msg = "new-file entry";
                }
                break;

            default:
                msg = "unknown tag";
                break;
        }
    }

    if (msg == nullptr && !input.empty()) {
        msg = "invalid tag";
    }

    Status result;
    if (msg != nullptr) {
        result = Status::Corruption("VersionEdit", msg);
    }
    return result;
}
```