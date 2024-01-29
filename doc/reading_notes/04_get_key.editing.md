- [查询一个 Key](#查询一个-key)
  - [查找 Key 的入口: DBImpl::Get(const ReadOptions\& options, const Slice\& key, std::string\* value)](#查找-key-的入口-dbimplgetconst-readoptions-options-const-slice-key-stdstring-value)
    - [从 MemTable 中查找](#从-memtable-中查找)
    - [从 Immutable MemTable 中查找](#从-immutable-memtable-中查找)
    - [从 SST 中查找](#从-sst-中查找)


# 查询一个 Key

## 查找 Key 的入口: DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value)

LevelDB 中查询一个`Key`的接口为`Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value);`，其实现如下。

```cpp
Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    Status s;
    MutexLock l(&mutex_);

    // 如果 options 中指定了 Snapshot，就使用该 Snapshot。
    // 否则的话，隐式的创建一个最新的 Snapshot，从该 Snapshot 
    // 中查询 Key。
    SequenceNumber snapshot;
    if (options.snapshot != nullptr) {
        snapshot = static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
    } else {
        snapshot = versions_->LastSequence();
    }

    // Key 可能存在 3 个地方:
    //   - MemTable
    //   - Immutable MemTable
    //   - SST
    // 这 3 个地方都需要查，先把它们的引用计数加 1，防止中途被销毁。
    MemTable* mem = mem_;
    MemTable* imm = imm_;
    Version* current = versions_->current();
    mem->Ref();
    if (imm != nullptr) imm->Ref();
    current->Ref();

    bool have_stat_update = false;
    Version::GetStats stats;

    {
        // 进入查询流程了，对 Memtable, Immutable Memtable 和 SST 只会做读取操作，
        // 不会做写操作，所以可以先释放锁，允许其他线程继续写入。
        mutex_.Unlock();
        
        // 基于 UserKey 和 Snapshot 构造一个 LookupKey，使用该 LookupKey 来做查询。
        LookupKey lkey(key, snapshot);
        if (mem->Get(lkey, value, &s)) {
            // 优先在 MemTable 中查找，如果查找成功，就不会在 Immutable MemTable 和 SST 中查找了。
        } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
            // MemTable 中没找到，就在 Immutable MemTable 中查找。
        } else {
            // MemTable 和 Immutable MemTable 都没找到，就在 SST 中查找。
            // 把 have_stat_update 标记为 true，表示在 SST 中进行查找了。
            // 在 SST 中查找完后，会将查找的额外信息存放到 stats 中:
            //   - 读取了哪个 SST
            //   - 读取到 SST 位于哪一层
            s = current->Get(options, lkey, value, &stats);
            have_stat_update = true;
        }
        mutex_.Lock();
    }

    // 如果是从 SST 中查找的 Key，并且该 SST 的累计 Seek 次数已经超过了阈值，
    // 那么就会触发 Compaction。
    if (have_stat_update && current->UpdateStats(stats)) {
        MaybeScheduleCompaction();
    }

    // 查找完毕，将 MemTable, Immutable MemTable 和 SST 的引用计数减 1。
    mem->Unref();
    if (imm != nullptr) imm->Unref();
    current->Unref();
    return s;
}
```

查询一个`Key`的流程如下:

### 从 MemTable 中查找

使用`MemTable::Get(const LookupKey& key, std::string* value, Status* s)`优先在`MemTable`中查找，如果查找成功，就不会在`Immutable MemTable`和`SST`中查找了。

`MemTable::Get(const LookupKey& key, std::string* value, Status* s)`的具体实现可移步参考[大白话解析LevelDB：MemTable](https://blog.csdn.net/sinat_38293503/article/details/134698711?spm=1001.2014.3001.5502#MemTableGet_214)。

### 从 Immutable MemTable 中查找

`Immutable Table`其实就是`MemTable`，只是为了与`MemTable`做区分，把名称叫做`Immutable MemTable`，但代码实现中，`Immutable MemTable`与`MemTable`都是`Class MemTable`。

```cpp
MemTable* mem = mem_;
MemTable* imm = imm_;
```

所以，`Immutable MemTable`的查找实现与`MemTable`的查找实现是同一个接口，不再赘述啦。

### 从 SST 中查找

如果前面在`MemTable`和`Immutable MemTable`中都没找到目标 Key，就只能从`SST`中查找了。

LevelDB 认为，如果某个`SST`被读取了很多次，就应该对其进行`Compaction`，将其与下层(level+1)的`SST`合并，这样就可以减少`SST`的数量。

那么减少`SST`的数量有什么好处呢？

1. 提高磁盘空间的使用效率。

