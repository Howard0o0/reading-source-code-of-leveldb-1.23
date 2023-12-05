# MemTable落盘为SST文件

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

- 将`MemTable`落盘成`SSTable`文件
- 构建新的`Version`，包含新`SSTable`文件的`MetaData`等信息
- 清理不再需要的文件

现在我们逐一分析这3部分的实现。

### 将`MemTable`落盘成`SSTable`文件

