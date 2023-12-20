# MemTable落盘为SST文件

- [MemTable落盘为SST文件](#memtable落盘为sst文件)
  - [什么是`Compact MemTable`](#什么是compact-memtable)
  - [什么时候触发`Compact MemTable`](#什么时候触发compact-memtable)
  - [如何触发`Compact MemTable`](#如何触发compact-memtable)
  - [`Compact MemTable`的过程](#compact-memtable的过程)
    - [将`MemTable`落盘成`SST`文件](#将memtable落盘成sst文件)
      - [将`MemTable`生成一个新的`SST`文件:](#将memtable生成一个新的sst文件)
      - [挑选合适的 level-i 用于放置新的`SST`](#挑选合适的-level-i-用于放置新的sst)
      - [将新`SST`的`MetaData`记录到`VersionEdit`中](#将新sst的metadata记录到versionedit中)


LevelDB中有两种`Compaction`，一种是`Compact MemTable`，另一种是`Compact SST`。`Compact MemTable`是将`MemTable`落盘为SST文件，`Compact SST`是将多个SST文件合并为一个SST文件。

本章讲述的是`Compact MemTable`的过程。

## 什么是`Compact MemTable`

LevelDB在将`Key-Value`写入`MemTable`的过程中，会先检查当前`MemTable`的大小是否有达到阈值。如果达到阈值，则创建一个新的`MemTable`，并且在后台线程中将旧的`MemTable`写入磁盘。这个过程就是`Compact MemTable`。

## 什么时候触发`Compact MemTable`

对应到代码中，`DBImpl::Write`在写`MemTable`前，会先通过`MakeRoomForWrite`检查当前`MemTable`的大小是否达到阈值。如果达到阈值，则会创建一个新的`MemTable`，并且在后台线程中将旧的`MemTable`写入磁盘，即`Compact MemTable`。

```c++
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    // ...

    // 在 MakeRoomForWrite 中，
    // 会检查当前 MemTable 的大小是否达到阈值。
    // 如果达到阈值，则会创建一个新的 MemTable，
    // 并且在后台线程中将旧的 MemTable 写入磁盘。
    Status status = MakeRoomForWrite(updates == nullptr);
    
    // 将 Key-Value 写入 MemTable
    // ...
}
```

我们跳入`MakeRoomForWrite`函数中，再看看触发`Compact MemTable`的具体细节。

```c++
// 检查是否可以写入MemTable，
// 如果暂时不能写入，则会同步阻塞，
// 直到可以写入为止。
Status DBImpl::MakeRoomForWrite(bool force) {
    mutex_.AssertHeld();
    assert(!writers_.empty());
    bool allow_delay = !force;
    Status s;
    while (true) {
        if (!bg_error_.ok()) {
            // 检查后台线程是否出错，
            // 如果出错，则直接返回错误。
            s = bg_error_;
            break;
        } else if (allow_delay && versions_->NumLevelFiles(0) >=
                                      config::kL0_SlowdownWritesTrigger) {
            // 检查 Level-0 的文件数是否达到缓写阈值，
            // 如果达到阈值，则延迟 1 毫秒写入。
            // 通过延迟写入，降低写入速度，给后台Compaction线程腾出资源。
            mutex_.Unlock();
            env_->SleepForMicroseconds(1000);
            allow_delay = false;  // Do not delay a single write more than once
            mutex_.Lock();
        } else if (!force && (mem_->ApproximateMemoryUsage() <=
                              options_.write_buffer_size)) {
            // 如果 MemTable 还没写满，则检查结束。
            break;
        } else if (imm_ != nullptr) {
            // 如果 MemTable 已经写满，但是老的 MemTable 还没完成落盘，
            // 则等待老的 MemTable 落盘完成。
            Log(options_.info_log, "Current memtable full; waiting...\n");
            background_work_finished_signal_.Wait();
        } else if (versions_->NumLevelFiles(0) >=
                   config::kL0_StopWritesTrigger) {
            // 检测到 Level-0 的文件数达到停写阈值，则阻塞等待直到后台的Compaction线程完成。
            // 注意与上面的缓写阈值区分开，
            // 缓写阈值是只是延迟1ms就可以继续写入，
            // 而停写阈值是必须等待Compaction线程完成才能继续写入。
            Log(options_.info_log, "Too many L0 files; waiting...\n");
            background_work_finished_signal_.Wait();
        } else {
            // 此处是我们需要关注的重点，触发 Compact MemTable 的地方。

            // 一个新的 MemTable 需要对应一个新的 WAL(Write Ahead Log)，
            // 生成新的 MemTable 前先生成新的 WAL。
            // WAL的逻辑我们可以先不关注，后面再专门讲WAL相关的内容。
            // 此处我们只需要简单知道 WAL 是用来记录每次写入 MemTable 的日志，
            // 用于容灾。
            // 当发生宕机的时候，我们可以通过redo WAL来恢复数据。
            assert(versions_->PrevLogNumber() == 0);
            uint64_t new_log_number = versions_->NewFileNumber();
            WritableFile* lfile = nullptr;

            s = env_->NewWritableFile(LogFileName(dbname_, new_log_number),
                                      &lfile);
            if (!s.ok()) {
                // Avoid chewing through file number space in a tight loop.
                versions_->ReuseFileNumber(new_log_number);
                break;
            }
            delete log_;
            delete logfile_;
            logfile_ = lfile;
            logfile_number_ = new_log_number;
            log_ = new log::Writer(lfile);

            // 创建 WAL 结束，现在开始创建新的 MemTable。
            // 先将当前的 MemTable 转变成 Immutable MemTable， 也就是
            // Read-Only的 MemTable。
            // 转换的过程很简单，将 imm_ 指向当前的 MemTable，再将
            // mem_ 指向一个新的 MemTabl e即可
            imm_ = mem_;
            has_imm_.store(true, std::memory_order_release);
            mem_ = new MemTable(internal_comparator_);
            mem_->Ref();
            force = false;  // Do not force another compaction if have room

            // 创建新的 MemTable 结束，
            // 通过 MaybeScheduleCompaction 触发后台的 Compact MemTable 线程。
            MaybeScheduleCompaction();
        }
    }
    return s;
}
```

`MakeRoomForWrite`会检查很多条件。检查L0的`SST`是否太多，检查当前`MemTable`是否写满等。我们关注的是当`MemTable`写满时，触发`Compact MemTable`的逻辑。

通过`MakeRoomForWrite`的实现细节我们可以看到，当`MemTable`写满时，会创建一个新的`MemTable`，并且将旧的`MemTable`转换为`Immutable MemTable`，然后通过调用`MaybeScheduleCompaction`来触发后台的`Compact MemTable`线程。

综上，`DBImpl::Write`在写`MemTable`前，会先通过`MakeRoomForWrite`检查当前`MemTable`的大小是否达到阈值。如果达到阈值，则会创建一个新的`MemTable`，并通过`MaybeScheduleCompaction`触发后台的`Compact MemTable`线程。

## 如何触发`Compact MemTable`

有的同学是不是以为`MaybeScheduleCompaction()`里就会直接根据条件调用`Compact MemTable`的实现函数了呢？

还木有那么快，让我们先看看`MaybeScheduleCompaction()`的实现细节。

```c++
// 函数的名字概括的很恰当，
// 如果条件满足则安排后台线程执行 Compaction.
void DBImpl::MaybeScheduleCompaction() {
    mutex_.AssertHeld();
    if (background_compaction_scheduled_) {
        // 如果之前安排的后台线程还没执行完，则直接返回，无需再次安排。
    } else if (shutting_down_.load(std::memory_order_acquire)) {
        // 如果DB正在关闭，不需要安排了，直接返回。
    } else if (!bg_error_.ok()) {
        // 如果后台线程有出错，不需要安排了，直接返回。
    } else if (imm_ == nullptr && manual_compaction_ == nullptr &&
               !versions_->NeedsCompaction()) {
        // 检查compaction的条件，没有满足的，不需要安排。
        // 3种条件满足其一即可：
        //      - imm_ != nullptr，表示当前有等待执行的 MemTable Compaction.
        //      - manual_compaction_ != nullptr，表示当前有待执行的手动Compaction.
        //      - versions_->NeedsCompaction()，表示当前有待执行的 SST Compaction.
    } else {
        // 上面的三个条件至少有一个命中了，把 background_compaction_scheduled_ 标志位设置为 true，
        // 以免重复安排后台线程执行 Compaction。
        background_compaction_scheduled_ = true;
        // 将 BGWork 方法加入线程池中执行。
        // 具体需要执行什么类型的Compaction，BGWork 里再做判断。
        env_->Schedule(&DBImpl::BGWork, this);
    }
}
```

通过查看``MaybeScheduleCompaction()`的实现细节，我们知道了有3种情况可以安排后台线程执行`Compaction`。

- `imm_ != nullptr`，表示当前有等待执行的`MemTable Compaction`。
- `manual_compaction_ != nullptr`，表示当前有待执行的手动`Compaction`。
- `versions_->NeedsCompaction()`，表示当前有待执行的`SST Compaction`。

我们现在关心的是`MemTable Compaction`。但现在还是没有看到是怎么触发`MemTable Compaction`的，需要继续查看`DBImpl::BGWork`的实现细节。

对`DBImpl::BGWork`的实现感兴趣的同学，可以移步学习[大白话解析LevelDB: Env 跨平台运行环境的封装](TODO)

```c++
void DBImpl::BGWork(void* db) {
    reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}
```

诶？为什么不直接调用`DBImpl::BackgroundCall`呢？还要通过`DBImpl::BGWork`包装一层？

这是因为`env_->Schedule`需要传入一个C风格的函数指针，也就是要么是静态的成员函数，要么是全局函数。而`DBImpl::BackgroundCall`是一个非静态的成员函数，所以需要通过`DBImpl::BGWork`这个静态成员函数包装一层。

OK，那我们继续看`DBImpl::BackgroundCall`的实现细节。

```c++
void DBImpl::BackgroundCall() {
    MutexLock l(&mutex_);
    // LevelDB中大量使用了assert，有2方面作用。
    //      - 用于检查代码逻辑是否正确，如果assert失败，在UT中就可以及时发现。
    //      - 增加可读性，告诉读者该函数执行的前提条件。
    assert(background_compaction_scheduled_);
    if (shutting_down_.load(std::memory_order_acquire)) {
        // 如果DB正在关闭，就不需要执行后台线程了。
    } else if (!bg_error_.ok()) {
        // 如果之前的后台线程执行出错了，
        // 就不要再继续执行了，因为很有可能会遇到相同的错误，
        // 重复执行也没啥意义，浪费资源。
    } else {
        // 终于找到入口了，感人 
        BackgroundCompaction();
    }

    background_compaction_scheduled_ = false;

    // 执行完上面的BackgroundCompaction后，某一层的SST文件可能会超过阈值，
    // 递归调用MaybeScheduleCompaction，每次Compaction结束后都检查下是否需要再次Compaction，
    // 直到不再满足Compaction条件为止。
    MaybeScheduleCompaction();

    // Compaction结束后，唤醒被 MakeRoomForWrite 阻塞的写线程。
    background_work_finished_signal_.SignalAll();
}
```

诶？`DBImpl::MaybeScheduleCompaction`里不是已经检查过`shutting_down_`了吗，为什么这里还要再检查一次呢？

因为有可能`DBImpl::MaybeScheduleCompaction`里检查的时候还没有关闭DB，执行到`DBImpl::BackgroundCall`的时候才关闭。所以再检查一下，能提高拦截的概率。

`DBImpl::BackgroundCall`仍然是一层封装，完成`Compaction`后递归的调用`MaybeScheduleCompaction`，直到不再满足`Compaction`条件为止。

不过我们要找的是`Compact MemTable`的入口，现在只找到了`Compaction`的入口`BackgroundCompaction`，那我们继续看看`BackgroundCompaction`的实现细节。

```c++
void DBImpl::BackgroundCompaction() {
    mutex_.AssertHeld();

    // woo! 终于找到`Compact MemTable`的入口了。
    if (imm_ != nullptr) {
        CompactMemTable();
        return;
    }

    // 后面是`Compact SST`与`Manual Compaction`的逻辑，我们暂且先不看。
    // 先专注于`Compact MemTable`。
    // ...
}
```

Nice, 终于找到`Compact MemTable`的入口`DBImpl::CompactMemTable`了。

那么现在我们可以得出`Compact MemTable`的触发路径了：

```c++
DBImpl::Write 
    -> MakeRoomForWrite 
        -> MaybeScheduleCompaction
            -> DBImpl::BGWork
                -> DBImpl::BackgroundCall
                    -> DBImpl::CompactMemTable
```

## `Compact MemTable`的过程

从上面一小节的分析我们得到了`Compact MemTable`的入口`DBImpl::CompactMemTable`，那么我们现在就可以开始分析`Compact MemTable`的过程了。

```c++
void DBImpl::CompactMemTable() {
    mutex_.AssertHeld();
    assert(imm_ != nullptr);

    // 创建一个VersionEdit对象，用于记录从当前版本到新版本的所有变化。
    // 在CompactMemTable中主要是记录新生成的SSTable文件的MetaData.
    VersionEdit edit;

    // 获取当前版本。
    // LevelDB维护了一个VersionSet，是一个version的链表。
    // 将VersionEdit Apply到base上，就可以得到一个新的version.
    Version* base = versions_->current();

    // 增加base的引用计数，防止其他线程将该version删除。
    // 比如Compact SST的过程中，会将一些version合并成一个version，
    // 并删除中间状态的version。
    base->Ref();

    // 将MemTable保存成新的SSTable文件，
    // 并将新的SSTable文件的MetaData记录到edit中。
    Status s = WriteLevel0Table(imm_, &edit, base);

    // base可以释放了。
    base->Unref();

    // 再检查下是不是在shutdown，
    // 如果在shutdown的话就及时结束当前的CompactMemTable。
    if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
        s = Status::IOError("Deleting DB during memtable compaction");
    }

    if (s.ok()) {
        // SST构建成功，
        // 把WAL相关信息记录到VersionEdit中。

        // 新的SST创建好后，旧的WAL不再需要了，
        // 所以设置为0
        edit.SetPrevLogNumber(0);
        // 设置新的SST对应的WAL编号
        edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
        
        // 将VersionEdit应用到当前Version上，
        // 产生一个新的Version，加入VersionSet中，
        // 并将新的Version设置为当前Version。
        s = versions_->LogAndApply(&edit, &mutex_);
    }

    if (s.ok()) {
        // 新Version构建成功，
        // 则Compact MemTable就完成了，
        // 可以做一些清理工作了。         

        // 将Immutable MemTable释放
        imm_->Unref();
        imm_ = nullptr;
        has_imm_.store(false, std::memory_order_release);

        // 移除不再需要的文件
        RemoveObsoleteFiles();
    } else {
        RecordBackgroundError(s);
    }
}
```

简单概括一下，`DBImpl::CompactMemTable`做了3件事，分别是：

- 将`MemTable`落盘成`SST`文件
- 构建新的`Version`，包含新`SSTable`文件的`MetaData`等信息
- 清理不再需要的文件

现在我们逐一分析这3部分的实现。

### 将`MemTable`落盘成`SST`文件

简单概括下`DBImpl::WriteLevel0Table`：

- 生成一个新的`SST`文件: `BuildTable(dbname_, env_, options_, table_cache_, iter, &meta)`
- 挑选出一个合适的`level-i`，用于放置新的`SST`文件: `PickLevelForMemTableOutput(min_user_key, max_user_key);`
- 将新的`SST`文件的`MetaData`记录到`VersionEdit`中: `edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest)`
    - `SST`所在的`level`
    - `SST`的编号
    - `SST`的大小
    - `SST`的最小`Key`和最大`Key`

```c++
Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base) {
    mutex_.AssertHeld();
    // 开始计时SST的构建时间，
    // 会记录到stats里。
    const uint64_t start_micros = env_->NowMicros();

    // 创建一个FileMetaData对象，用于记录SST的元数据信息。
    // 比如SST的编号、大小、最小Key、最大Key等。
    FileMetaData meta;
    // NewFileNumber()的实现很简单，就是一个自增的计数器。
    meta.number = versions_->NewFileNumber();

    // 把新SST的编号记录到pending_outputs_中，
    // 是为了告诉其他线程，这个SST正在被构建中，
    // 不要把它误删除了。
    // 比如手动Compact的过程中会检查pending_outputs_，
    // 如果待压缩的目标SST文件存在于pending_outputs_中，
    // 就终止Compact。
    pending_outputs_.insert(meta.number);

    // 创建一个MemTable的Iterator，
    // 用于读取MemTable中的所有KV数据。
    Iterator* iter = mem->NewIterator();
    Log(options_.info_log, "Level-0 table #%llu: started", (unsigned long long)meta.number);

    Status s;
    {
        mutex_.Unlock();
        // 遍历 MemTable 中的所有 KV 数据，将其写入 SSTable 文件中
        s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
        mutex_.Lock();
    }

    Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s", (unsigned long long)meta.number,
        (unsigned long long)meta.file_size, s.ToString().c_str());
    delete iter;

    // SST已经构建好了，从pending_outputs_中移除。
    pending_outputs_.erase(meta.number);

    // Note that if file_size is zero, the file has been deleted and
    // should not be added to the manifest.
    int level = 0;
    // s.ok()但是meta.file_size为0的情况很罕见，但存在。
    // 比如用户传入了特定的过滤逻辑，把MemTable的所有kv都过滤掉了，
    // 此时BuildTable成功，但是SST的内容为空。
    if (s.ok() && meta.file_size > 0) {
        const Slice min_user_key = meta.smallest.user_key();
        const Slice max_user_key = meta.largest.user_key();
        if (base != nullptr) {
            // 挑选出一个合适的 level，用于放置新的 SSTable
            level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
        }
        /* 新增了一个 SSTable，因此需要更新 VersionSet::new_files_ 字段 */
        // 新增了一个SST，把metadata记录到VersionEdit中。
        edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
    }

    // 我们可以从leveldb的log或者api接口里获取stats。
    CompactionStats stats;

    // 将Build SST的时间开销与SST大小记录到stats里
    stats.micros = env_->NowMicros() - start_micros;
    stats.bytes_written = meta.file_size;
    // 记录New SST最终被推到哪个level
    stats_[level].Add(stats);
    return s;
}
```

#### 将`MemTable`生成一个新的`SST`文件:

现在我们来看下`BuildTable`的实现细节。

简单概括一下：

1. 根据`meta->number`创建一个`SST`文件。
2. 创建一个`TableBuilder`对象，通过它将`MemTable`里所有的kv写入到`SST`文件里。
3. 检查是否有错误。
4. 如果有错误或者生成的`SST`文件为空，删除该`SST`。

```c++
Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
    Status s;
    // 初始化SST的大小为0
    meta->file_size = 0;

    // 将MemTable的迭代器指向第一个元素
    iter->SeekToFirst();

    // 生成SST的文件名
    // 在LevelDB中，SSTable的文件名的格式为/dbpath/number.sst，其中：
    // 
    //      - /dbpath/是数据库的路径，对应于dbname参数。
    //      - number是SSTable的文件编号，对应于meta->number参数。
    //      - .sst是文件的扩展名，表示这是一个SSTable文件。
    std::string fname = TableFileName(dbname, meta->number);
    if (iter->Valid()) {

        // 创建SST文件
        WritableFile* file;
        s = env->NewWritableFile(fname, &file);
        if (!s.ok()) {
            return s;
        }

        // 创建一个TableBuilder对象，
        // 用于将MemTable中的数据写入到SST文件中
        TableBuilder* builder = new TableBuilder(options, file);

        // MemTable中的kv是按照升序的，
        // 所以第一个key就是最小的key，最后一个key就是最大的key
        meta->smallest.DecodeFrom(iter->key());


        // 通过TableBuilder对象将
        // 所有kv写入到SST文件中
        Slice key;
        for (; iter->Valid(); iter->Next()) {
            key = iter->key();
            builder->Add(key, iter->value());
        }

        // 最后一个key就是最大的key
        if (!key.empty()) {
            meta->largest.DecodeFrom(key);
        }

        // Finish and check for builder errors
        // 完成收尾工作，并在metadata里记录SST的大小
        s = builder->Finish();
        if (s.ok()) {
            meta->file_size = builder->FileSize();
            assert(meta->file_size > 0);
        }
        delete builder;

        // Finish and check for file errors
        // 把buffer里剩余的数据写入到文件中
        if (s.ok()) {
            s = file->Sync();
        }
        // 关闭文件，释放文件描述符
        if (s.ok()) {
            s = file->Close();
        }
        delete file;
        file = nullptr;

        if (s.ok()) {
            // Verify that the table is usable
            // 创建一个SST的迭代器，用于检查生成的SST是否可用
            Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size);
            s = it->status();
            delete it;
        }
    }

    // Check for input iterator errors
    // 检查MemTabel的迭代器是否有错误，
    // 如果有的话，说明MemTable可能会有
    // 部分数据没有刷到SST里。
    if (!iter->status().ok()) {
        s = iter->status();
    }

    // 如果SST和MemTable都没有问题，
    // 并且该SST不为空，就保留这个SST。
    // 否则的话，删除该SST。
    // 这里可能会有些有疑惑，怎么会有s.ok()但是
    // SST为空的情况呢？
    // `builder->Add(key, iter->value()`里
    // 会检查用户是否设置了filter，如果有filter就
    // 按照用户设置的filter来过滤掉一些kv，这样
    // 就会有MemTable中的所有kv都被过滤掉的情况。
    if (s.ok() && meta->file_size > 0) {
        // Keep it
    } else {
        env->RemoveFile(fname);
    }
    return s;
}
```

由于文件写入在不同平台(比如posix && win)需要使用不同的接口，所以LevelDB将文件写入相关的操作抽象出了一个接口`WritableFile`，如下：

```c++
class LEVELDB_EXPORT WritableFile {
   public:
    WritableFile() = default;

    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;

    virtual ~WritableFile();

    virtual Status Append(const Slice& data) = 0;
    virtual Status Close() = 0;
    virtual Status Flush() = 0;
    virtual Status Sync() = 0;
};
```

对于`posix`上`WritableFile`的实现，请移步阅读[大白话解析LevelDB: WritableFile 接口](https://blog.csdn.net/sinat_38293503/article/details/135118828)。

`TableBuilder`生成`SST`的篇幅较多，请移步阅读[大白话解析LevelDB: TableBuilder](https://blog.csdn.net/sinat_38293503/article/details/135043003)。

#### 挑选合适的 level-i 用于放置新的`SST`

PickLevelForMemTableOutput

#### 将新`SST`的`MetaData`记录到`VersionEdit`中