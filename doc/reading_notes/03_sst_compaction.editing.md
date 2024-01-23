# SST Compaction

- [SST Compaction](#sst-compaction)
  - [什么是`Compact SST`](#什么是compact-sst)
  - [什么时候触发`Compact SST`](#什么时候触发compact-sst)
    - [第一处 读取 Key 的时候](#第一处-读取-key-的时候)
    - [第二处 使用迭代器遍历数据库时](#第二处-使用迭代器遍历数据库时)
    - [第三处 写入 Key 时](#第三处-写入-key-时)
    - [第四处 刚打开数据库时](#第四处-刚打开数据库时)
  - [如何进行`Compact SST`](#如何进行compact-sst)
    - [1. 计算`Compaction`范围](#1-计算compaction范围)
    - [2. 进行`Compaction`](#2-进行compaction)


LevelDB中有两种`Compaction`，一种是`Compact MemTable`，另一种是`Compact SST`。`Compact MemTable`是将`MemTable`落盘为SST文件，`Compact SST`是将多个SST文件合并为一个SST文件。

本章讲述的是`Compact SST`的过程。

## 什么是`Compact SST`

相比于`Compact MemTable`，`Compact SST`复杂的多。首先看一下一次`SST Compaction`的示意图。

![SST Compaction](./image/image-2.png)

Level-0 中浅蓝色的三个 SST 文件，加上 Level-1 中的绿色的 SST 文件，这 4 个文件进行了合并，输出两个新的 SST 文件，替换原有的 SST 文件。

## 什么时候触发`Compact SST`

Compaction 的入口为`MaybeScheduleCompaction()`，`MaybeScheduleCompaction()`里面会判断是需要`Compact SST`还是`Compact MemTable`。

我们来看下有哪些地方调用了`MaybeScheduleCompaction()`。

### 第一处 读取 Key 的时候

我们调用`DBImpl::Get()`读取某个`Key`的时候，LevelDB 会按照`MemTable => Immutable MemTable => SST`的顺序查找，如果在`MemTable`或者`Immutable MemTable`中找到了，那么就不会触发 Compaction。但如果`Key`是在 SST 中找到的，这个 SST 的`allowed_seeks`就会减 1。当`allowed_seeks`为 0 时，就表示这个 SST 需要`Compact`了。

所以在`DBImpl::Get()`中，如果是从 SST 中查找的 Key，就需要调用一下`MaybeScheduleCompaction()`，尝试触发 Compaction。

```cpp
Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    
    // ...

    bool have_stat_update = false;
    Version::GetStats stats;

    {
        mutex_.Unlock();
        
        LookupKey lkey(key, snapshot);
        if (mem->Get(lkey, value, &s)) {
            // 从 MemTable 中查找成功，
            // 不会触发 Compaction。
        } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
            // 从 Immutable MemTable 中查找成功，
            // 也不会触发 Compaction。
        } else {
            // 如果查找 SST 了，有可能会触发 Compaction，
            s = current->Get(options, lkey, value, &stats);
            have_stat_update = true;
        }
        mutex_.Lock();
    }

    // 如果是从 SST 中查找的 Key，并且该 SST 的 Seek 次数
    // 已经超过了阈值，那么就会触发 Compaction。
    if (have_stat_update && current->UpdateStats(stats)) {
        MaybeScheduleCompaction();
    }
    
    // ...
    return s;
}
```

### 第二处 使用迭代器遍历数据库时

每当调用一次`it->Next()`或者`it->Prev()`移动迭代器时，迭代器内部都会调用一次`DBIter::ParseKey()`，将当前`Key`解析出来。

而在`DBIter::ParseKey()`中，会定期采样当前 Key，看看这个`Key`是否存在于多个`SST`中。如果是的话，就会将这个`Key`所在的`SST`的`allowed_seeks`减 1，然后调用`MaybeScheduleCompaction()`尝试触发 Compaction。

这样做的目的是定期检查`SST`中的`Key`是否存在于多个`SST`中，如果是的话，就通过`Compaction`将这个`Key`所在的`SST`合并到更高 Level 的`SST`中，这样就可以减少`SST`的数量，提高读取效率。

```c++
inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
    Slice k = iter_->key();

    // 当一个 iterator 已读取的数据大小超过 bytes_until_read_sampling_ 后，
    // 就会用当前 key 采一次样，查看这个 key.user_key 是否存在于多个(两个及以上) SST 
    // 中。如果是的话，就把 key 所在的 SST.allowed_seeks 减 1，然后调用
    // MaybeScheduleCompaction() 尝试触发 Compaction。
    size_t bytes_read = k.size() + iter_->value().size();
    while (bytes_until_read_sampling_ < bytes_read) {
        bytes_until_read_sampling_ += RandomCompactionPeriod();
        db_->RecordReadSample(k);
    }
    assert(bytes_until_read_sampling_ >= bytes_read);
    bytes_until_read_sampling_ -= bytes_read;

    if (!ParseInternalKey(k, ikey)) {
        status_ = Status::Corruption("corrupted internal key in DBIter");
        return false;
    } else {
        return true;
    }
}

void DBImpl::RecordReadSample(Slice key) {
    MutexLock l(&mutex_);
    if (versions_->current()->RecordReadSample(key)) {
        MaybeScheduleCompaction();
    }
}
```

### 第三处 写入 Key 时

写入`Key`(进`MemTable`)之前，会在`DBImpl::MakeRoomForWrite()`里检查`MemTable`是否已满。如果已满，就会调用`MaybeScheduleCompaction()`尝试触发 `Compaction`。此处的`Compaction`指的是`Compact MemTable`，在此就不详细赘述了，忘记的同学可以回头参考[大白话解析LevelDB 2: MemTable 落盘为 SST 文件](https://blog.csdn.net/sinat_38293503/article/details/135662037#Compact_MemTable_12)。

### 第四处 刚打开数据库时

在`DBImpl::Open()`中，会调用`MaybeScheduleCompaction()`尝试触发`Compaction`。

刚打开数据的时候为什么需要尝试触发`Compaction`呢？

因为当数据库上次关闭时，可能还有些没完成的`Compaction`，比如`Compaction`进行中途机器断电了。

所以当数据库打开时，需要尝试触发一次`Compaction`，检查下有没有未完成的`Compaction`。

```c++
Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
    // ...
    // 读取数据库文件，恢复数据库状态。
    Status s = impl->Recover(&edit, &save_manifest);
    // ...
    if (s.ok()) {
        impl->RemoveObsoleteFiles();
        // 当数据库关闭时，可能有些还没完成的 Compaction。
        // 所以打开数据库时尝试触发一次 Compaction，检查
        // 下有没有未完成的 Compaction。
        impl->MaybeScheduleCompaction();
    }
    // ...
    return s;
}
```

## 如何进行`Compact SST`

在`MaybeScheduleCompaction()`中，会通过`versions_->NeedsCompaction()`判断是否满足`Compaction`条件。

若条件满足，会将`background_compaction_scheduled_`标志位设置为`true`，然后将`DBImpl::BGWork()`加入线程池中，在后台线程中进行`Compaction`。

```c++
void DBImpl::MaybeScheduleCompaction() {
    mutex_.AssertHeld();
    if (background_compaction_scheduled_) {
        // 已经有 Compaction 在后台线程中执行了。
    } else if (shutting_down_.load(std::memory_order_acquire)) {
        // 数据库正在被关闭，不再进行 Compaction。
    } else if (!bg_error_.ok()) {
        // 存在错误，不再进行 Compaction。
    } else if (imm_ == nullptr && manual_compaction_ == nullptr && !versions_->NeedsCompaction()) {
        // 不满足 Compaction 条件
    } else {
        // 满足 Compaction 条件，把 Compaction Job 加入到后台线程池中。
        background_compaction_scheduled_ = true;
        env_->Schedule(&DBImpl::BGWork, this);
    }
}
```

`DBImpl::BGWork()`只是层包装，最终会调用到`DBImpl::BackgroundCompaction()`，也就是`Compaction`的实现函数。

`DBImpl::BackgroundCompaction()`里先计算出本次`Compaction`的范围，然后调用`DoCompactionWork()`进行`Compaction`。

```c++
void DBImpl::BackgroundCompaction() {
    mutex_.AssertHeld();

    // 先判断是否存在 Immutable MemTable，如果存在，
    // 就将本次 Compaction 判定为 MemTable Compaction。
    if (imm_ != nullptr) {
        CompactMemTable();
        return;
    }

    // 否则为判定为 SST Compaction，进入 SST Compaction 的流程。

    Compaction* c;

    bool is_manual = (manual_compaction_ != nullptr);
    InternalKey manual_end;

    // 如果是用户主动发起的手动 Compaction，本次 Compaction 的范围
    // 是由用户指定的，需要从 manual_compaction_ 中读取，而不是由 LevelDB 计算得出。
    // 否则，本次 Compaction 的范围由`PickCompaction()`计算得出。
    // 无论是手动 Compaction 还是自动 Compaction，最终都会把 Compaction 所
    // 涉及的 SST 文件编号记录到`Compaction* c`对象中。
    //    c->inputs_[0] 中存放的是 Compaction Level 所涉及的 SST 文件编号。
    //    c->inputs_[1] 中存放的是 Compaction Level+1 所涉及的 SST 文件编号。
    if (is_manual) {
        ManualCompaction* m = manual_compaction_;
        c = versions_->CompactRange(m->level, m->begin, m->end);
        m->done = (c == nullptr);
        if (c != nullptr) {
            manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
        }
        Log(options_.info_log, "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
            m->level, (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
            (m->end ? m->end->DebugString().c_str() : "(end)"),
            (m->done ? "(end)" : manual_end.DebugString().c_str()));
    } else {
        //  由 leveldb 计算 Compaction 范围
        c = versions_->PickCompaction();
    }

    Status status;
    if (c == nullptr) {
        // 经过上面的计算，发现本次 Compaction 不需要进行，直接返回。
    } else if (!is_manual && c->IsTrivialMove()) {
        // IsTrivialMove() 表示本次 Compaction 只需要简单地将 SST 文件从
        // level 层移动到 level+1 层即可，不需要进行 SST 文件合并。

        // 在 TrivialMove 的情况下，level 层需要 Compaction 的 SST 文件
        // 只能有一个。
        assert(c->num_input_files(0) == 1);

        // 获取 level 层需要 Compaction 的第 0 个 SST 文件的元数据信息。 
        FileMetaData* f = c->input(0, 0);

        // 编辑 VersionEdit，将 level 层需要 Compact 的 SST 从 level 层移除，
        // 并将其添加到 level+1 层。
        c->edit()->RemoveFile(c->level(), f->number);
        c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest, f->largest);

        // Apply 该 VersionEdit，将其应用到当前 VersionSet 中。
        status = versions_->LogAndApply(c->edit(), &mutex_);
        if (!status.ok()) {
            RecordBackgroundError(status);
        }
        VersionSet::LevelSummaryStorage tmp;
        Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
            static_cast<unsigned long long>(f->number), c->level() + 1,
            static_cast<unsigned long long>(f->file_size), status.ToString().c_str(),
            versions_->LevelSummary(&tmp));
    } else {
        // 需要进行 SST 文件合并的 Compaction。

        // 构造一个 CompactionState 对象，用于记录本次 Compaction 
        // 需要新生成的 SST 信息。
        CompactionState* compact = new CompactionState(c);

        // 进行真正的 SST Compaction 操作，将 Compaction SST
        // 文件合并生成新的 SST 文件。
        status = DoCompactionWork(compact);
        if (!status.ok()) {
            RecordBackgroundError(status);
        }

        // Compaction 结束后的清理工作工作。
        CleanupCompaction(compact);

        // Compaction 完成后，Input Version 就不再需要了，将其释放。
        c->ReleaseInputs();

        // 移除数据库中不再需要的文件。
        RemoveObsoleteFiles();
    }
    delete c;

    if (status.ok()) {
        // 没有异常，不需要进行任何异常处理。
    } else if (shutting_down_.load(std::memory_order_acquire)) {
        // 如果当前正在关闭数据库，那错误就先不需要处理了，留到下次打开
        // 数据时再处理，先以最快的时间关闭数据库。
    } else {
        Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
    }

    if (is_manual) {
        ManualCompaction* m = manual_compaction_;
        if (!status.ok()) {
            // Compaction 失败了，需要把 m->done 标记为 true，
            // 防止重复 Compact 该范围。
            m->done = true;
        }
        if (!m->done) {
            // Compaction 完成，需要把 m->begin 更新为本次
            // Compaction 的结尾，以便下次继续 Compact。
            m->tmp_storage = manual_end;
            m->begin = &m->tmp_storage;
        }
        manual_compaction_ = nullptr;
    }
}
```

### 1. 计算`Compaction`范围

1. 如果是手动触发的`Compaction`，那么初始范围由用户指定，最终通过`versions_->CompactRange()`计算出`Compaction`的范围。
2. 如果是自动触发的`Compaction`，那么最终通过`versions_->PickCompaction()`计算出`Compaction`的范围。

`versions_->CompactRange()`的实现可移步参考[大白话解析LevelDB: VersionSet](https://blog.csdn.net/sinat_38293503/article/details/135661973#Compaction_VersionSetCompactRangeint_level_const_InternalKey_begin_const_InternalKey_end_932)
`versions_->PickCompaction()`的实现可移步参考[大白话解析LevelDB: VersionSet](https://blog.csdn.net/sinat_38293503/article/details/135661973#Compaction_VersionSetPickCompaction_745)

### 2. 进行`Compaction`

1. 如果计算出的`Compaction`范围是`nullptr`，表示当前不需要进行`Compaction`，直接返回。
2. 如果计算出的`Compaction`范围符合`TrivialMove`条件，表示只需要将`SST`文件从`level`层移动到`level+1`层即可，不需要进行`SST`文件合并。
3. 否则的话，就需要进行`SST`文件合并，`DoCompactionWork(c)`将`Compaction`范围内的`SST`文件合并为一个新的`SST`文件。
