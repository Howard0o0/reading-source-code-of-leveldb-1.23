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

    // 查找一个给定 key 的 value。
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

### Version::AddIterators(const ReadOptions&, std::vector<Iterator*>* iters)

### Version::UpdateStats(const GetStats& stats)

### Version::RecordReadSample(Slice key)

### Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inputs)

### Version::OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key)

### Version::PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key)

### Version::DebugString()
