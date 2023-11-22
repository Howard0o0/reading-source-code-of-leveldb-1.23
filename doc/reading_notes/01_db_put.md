# leveldb 的写入流程 -- 写memtable

leveldb写入kv的接口为`virtual leveldb::Status leveldb::DB::Put(const leveldb::WriteOptions &options, const leveldb::Slice &key, const leveldb::Slice &value)`
其实现如下：
```c++
// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(opt, &batch);
}
```

`DB::Put`只干两件事情

1. 将key-value放入一个`WriteBatch`中
2. 调用`DB::Write`将`WriteBatch`写入数据库

将key-value放入`WriteBatch`的实现如下：
```c++
void WriteBatch::Put(const Slice& key, const Slice& value) {
    WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeValue));
    PutLengthPrefixedSlice(&rep_, key);
    PutLengthPrefixedSlice(&rep_, value);
}
```

`WriteBatch`本质是一个`std::string`, `rep_`是数据本身, `WriteBatch`封装了一些方法来对`rep_`的内容进行插入和读取.
`rep_`的格式如下：

```
|<---- 8 bytes ---->|<-- 4 bytes -->|<--------- Variable Size -------->|
+-------------------+---------------+----------------------------------+
|    Sequence       |     Count     |              Records             |
+-------------------+---------------+----------------------------------+
```

`Sequence`是一个64位的整数。在LevelDB 中，`Sequence`是一个非常关键的概念，用于实现其存储和检索机制。每个存储在 LevelDB 中的键值对都与一个唯一的序列号（sequence number）相关联。序列号的作用和重要性可以从以下几个方面理解：

- 版本控制
    - LevelDB 支持多版本并发控制（MVCC），即允许存储同一个键的多个版本。
    - 序列号用于标记同一个键在不同时间点的值。当存储相同键的新数据时，LevelDB 会分配一个新的序列号，这使得LevelDB能够保留同一键的历史版本。
- 读写隔离
    - 在并发环境中，LevelDB 使用序列号来保证读写操作的隔离性。
    - 通过检查序列号，LevelDB 能够为读操作提供一个一致的视图，即使在有其他写入操作发生的时候。
- 事务日志(WAL)
    - LevelDB 使用写前日志（Write-Ahead Logging，WAL）来保证数据的持久性。每个写入操作先写入日志，再写入存储结构。
    - 序列号在这里用于确保即使在系统崩溃的情况下，也能正确地恢复数据，并保持数据的一致性。
- 压缩和合并(Compaction)
    - LevelDB 定期进行压缩和合并操作，以减少存储空间的使用，并优化读取性能。
    - 在这个过程中，序列号帮助 LevelDB 确定哪些旧版本的数据可以被安全删除。
- 快照(Snapshot)
    - LevelDB 支持快照功能，允许用户获取特定时间点的数据库状态。
    - 序列号用于实现这一点，通过记录创建快照时的序列号，LevelDB 可以提供该时刻所有键的视图。

`Count`是一个32位的定长整数(`fixed32`)，表示`rep_`中`Records`的数量.
`fixed32` 编码方式在 LevelDB 中用于存储固定长度的 32 位（4 字节）整数。以下是对这种编码方式的步骤说明。

**整数转换为二进制**

- 将整数转换为 32 位的二进制形式。
- 例如，十进制数字 `263` 可以表示为二进制 `00000000 00000000 00000001 00000111`。

**固定长度存储**

- 无论整数大小如何，`fixed32` 都将其存储为 32 位（4 字节）。
- 上面的示例中，`263` 将存储为 `00 00 01 07`（每个数字表示一个字节）。

**编码结果**

- 对于数字 `263`，其 `fixed32` 编码将是：`00 00 01 07`。

`fixed32` 编码提供了一种简单且高效的方式来存储固定大小的整数，保证了存储空间的一致性和预测性。这在需要处理大量固定大小数据的数据库系统中尤为重要。


`Records`是一个序列，每个元素是一个`Record`，`Record`的格式如下：

```
|<-- 1 byte -->|<---- Variable Size ---->|<---- Variable Size ---->|
+--------------+-------------------------+-------------------------+
|   Type       |        Key              |        Value            |
+--------------+-------------------------+-------------------------+
```

其中`Type`是一个1字节的整数，表示`Record`的类型。`Type`的取值如下

```c++
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };
```

对于key-value的操作, 有`Add`, `Put`, `Delete`三种.
leveldb总是将key-value的操作以日志追加的形式写入磁盘, 
对于每个key-value的操作, 都会生成一个`Record`,并且用一个`Type`表示该`Record`的操作类型.
`kTypeValue`表示该`Record`是一个新增的key-value, 对应的可能是`Add`与`Put`操作.
`kTypeDeletion`表示该`Record`是一个删除的key, 对应的是`Delete`操作.
leveldb将`Put`视为`Add`操作, 后期会进行数据合并的处理.
对于同一个key来说, leveldb只会保存最新的key-value, 旧的key-value会被删除.

`Key`和`Value`都是一个变长的字符串, 其格式如下:

```
|<-- Variable Size -->|<---- Variable Size ---->|
+---------------------+-------------------------+
|   Length            |        Data             |
+---------------------+-------------------------+
```

`Length`是一个`Varint`, 表示后面`Data`的长度。
LevelDB 使用`varint`编码来高效存储整数。以下是一个对数字 `300` 进行`varint`编码的步骤说明。

**转换为二进制**

- 十进制 `300` 在二进制中表示为 `100101100`.

**划分为 7 位一组**

- 从低位开始，每 7 位分为一组：`0010110` 和 `0000100`.

**添加控制位**

- 每组前添加一位，`1` 表示后面还有数据，`0` 表示这是最后一个字节。
- `0010110` -> `10010110` （因为后面还有字节，所以在前面加 `1`）
- `0000100` -> `00000100` （这是最后一个字节，所以在前面加 `0`）

**Varint 编码结果**

- `10010110 00000100`

在这个例子中，数字 `300` 被编码为两个字节：`10010110 00000100`。这种编码方法对于 LevelDB 来说非常重要，因为它可以节省存储空间并提高处理效率。通过 varint 编码，LevelDB 能够根据实际数值的大小动态地调整使用的字节数，这对于数据库系统来说是一个很大的优势。

`Data`中则是`Key`或`Value`的内容.


ok, 至此, `WriteBatch`的格式已经分析完毕.
我们看到leveldb使用了两种整数编码方式, `fixed32`和`varint`, 两种编码方式都是为了节省存储空间并提高处理效率.
那为什么要使用到两种编码方式呢, 他们适用的场景不同.

**`fixed32` 编码**

**定义**:
- `fixed32` 是一种固定长度的编码方式，使用 32 位（4 字节）来表示一个整数。

**适用场景**:
- **固定大小的数据**: 当数据大小固定且需要预测存储空间时，`fixed32` 非常适用。
- **性能要求**: 对于性能敏感的应用，读取固定长度的数据通常比可变长度数据更快。

**`varint`编码**

**定义**:
- `varint` 是一种可变长度的整数编码方式，根据整数大小使用不同数量的字节。

**适用场景**:
- **节省空间**: 当存储的整数大小变化较大时，`varint` 编码能有效节省空间。
- **数据大小波动大**: 在数据大小不固定且倾向于较小的情况下，使用 `varint` 可以减少存储空间的浪费。

**为什么需要两种编码**

- **优化存储和性能**: 通过选择适合每种数据特征的编码方式，LevelDB 在存储效率和读写性能之间实现平衡。
- **灵活性和适应性**: LevelDB 处理多种类型和大小的数据，这两种编码方式提供了更大的灵活性和适应性。

使用 `fixed32` 和 `varint` 两种编码方法使 LevelDB 能够高效且灵活地处理各种数据，满足不同场景的需求。

好了，关于`WriteBatch`的编码方式就讲到这, 回到`DB::Put`的第一步, 将key-value放入一个`WriteBatch`中.

```c++
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
    WriteBatch batch;
    batch.Put(key, value); // <= focus here
    return Write(opt, &batch);
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
    WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeValue));
    PutLengthPrefixedSlice(&rep_, key);
    PutLengthPrefixedSlice(&rep_, value);
}
```

`WriteBatch::Put(key, value)`就是把一对key-value放进WriteBatch里。

1. 将`WriteBatch`的`Count`加1
2. 将`kTypeValue`放入`WriteBatch`的`rep_`中
3. 将`key`放入`WriteBatch`的`rep_`中
4. 将`value`放入`WriteBatch`的`rep_`中

ok, `DB::Put`里的第一步已经分析完毕, 接下来看第二步, 调用`DB::Write`将`WriteBatch`写入数据库.

```c++
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    Writer w(&mutex_);
    w.batch = updates;
    w.sync = options.sync;
    w.done = false;

    MutexLock l(&mutex_);
    writers_.push_back(&w);
    while (!w.done && &w != writers_.front()) {
        w.cv.Wait();
    }
    if (w.done) {
        return w.status;
    }

    Status status = MakeRoomForWrite(updates == nullptr);
    uint64_t last_sequence = versions_->LastSequence();
    Writer* last_writer = &w;
    if (status.ok() &&
        updates != nullptr) {  // nullptr batch is for compactions
        WriteBatch* write_batch = BuildBatchGroup(&last_writer);
        WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
        last_sequence += WriteBatchInternal::Count(write_batch);

        // Add to log and apply to memtable.  We can release the lock
        // during this phase since &w is currently responsible for logging
        // and protects against concurrent loggers and concurrent writes
        // into mem_.
        {
            mutex_.Unlock();
            status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
            bool sync_error = false;
            if (status.ok() && options.sync) {
                status = logfile_->Sync();
                if (!status.ok()) {
                    sync_error = true;
                }
            }
            if (status.ok()) {
                status = WriteBatchInternal::InsertInto(write_batch, mem_);
            }
            mutex_.Lock();
            if (sync_error) {
                // The state of the log file is indeterminate: the log record we
                // just added may or may not show up when the DB is re-opened.
                // So we force the DB into a mode where all future writes fail.
                RecordBackgroundError(status);
            }
        }
        if (write_batch == tmp_batch_) tmp_batch_->Clear();

        versions_->SetLastSequence(last_sequence);
    }

    while (true) {
        Writer* ready = writers_.front();
        writers_.pop_front();
        if (ready != &w) {
            ready->status = status;
            ready->done = true;
            ready->cv.Signal();
        }
        if (ready == last_writer) break;
    }

    // Notify new head of write queue
    if (!writers_.empty()) {
        writers_.front()->cv.Signal();
    }

    return status;
}
```

这一大段代码看着是不是有点头疼, 别担心，我们拆成小块逐步分析.
先看开头的这小段代码
构造一个`Writer`对象, 把要写入的`WriteBatch`放进`writer`里.

```c++
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    Writer w(&mutex_);
    w.batch = updates; // 需要写入的batch
    w.sync = options.sync; // 是否需要同步写入磁盘
    w.done = false; // writer的工作是否已完成
    // ...
}
```

继续往下看
`DB`里有一个`mutex_`锁, 用来保护一些内变量的读写, 比如`writers_`就是其中之一.
先把`mutex_`拿到, 然后将开头构造好的`writer`放入`writers_`中.
`writers_`是一个`std::deque<Writer*>`, 所有线程构造出来的`writer`都会压入到`writers_`里, 排队等待执行。
排队执行的这个过程处理得很巧妙, `while (!w.done && &w != writers_.front())`的意思是如果当前`writer`还没执行完, 并且当前`writer`还没有变成`writers_`队列的头节点的话，就进入休眠，等待唤醒.
当前`writer`被唤醒后，看下自己的状态，是不是已经被别的`writer`帮忙把活干了，如果活已经被帮忙干好了, 就可以直接`return`了

```c++
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    // ...
    // 构造一个writer

    MutexLock l(&mutex_);
    writers_.push_back(&w);
    while (!w.done && &w != writers_.front()) {
        w.cv.Wait();
    }
    if (w.done) {
        return w.status;
    }

    // ...
}
```

如果`writer`醒来后发现自己的活还在, 那就继续往下走, `MakeRoomForWrite`

```c++
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    // ...
    // 构造一个writer

    // ...
    // 将writer放入writers_中
    // 如果不是队列头节点, 则进入休眠
    // 醒来后发现活还在, 继续往下走, 干活

    Status status = MakeRoomForWrite(updates == nullptr);

    // ...
}
```

`MakeRoomForWrite`的作用是确保有足够的空间来进行新的写入操作。具体来说，这个函数的逻辑可以分为以下几个步骤：

1. **检查是否需要强制切换memtable**：如果参数`force`为`true`，或者当前memtable的大小已经超过了阈值，那么就需要强制切换memtable。

```c++
if (force || mem_->ApproximateMemoryUsage() > options_.write_buffer_size)
```

2. **切换memtable**：将当前的memtable（memtable）转换为不可变memtable（immtable），然后创建一个新的memtable来存储新的写入操作。

```c++
if (imm_ != nullptr) {
    Log(options_.info_log, "Current memtable full; waiting...\n");
    while (imm_ != nullptr && bg_error_.ok()) {
        bg_cv_.Wait();
    }
}   
```

3. **触发后台线程**：如果当前没有后台线程在运行，那么就触发一个后台线程来将不可变memtable写入磁盘。

```c++
if (bg_error_.ok() && !bg_compaction_scheduled_ && imm_ != nullptr) {
    bg_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
}
```

4. **等待足够的空间**：如果写入操作的数量超过了一定的阈值，那么`MakeRoomForWrite`函数还会等待直到有足够的空间可以进行新的写入操作。这是通过一个循环来实现的，循环的条件是写入操作的数量超过了阈值。

```c++
while (bg_error_.ok() && writers_.size() > kL0_SlowdownWritesTrigger) {
    Log(options_.info_log, "Too many L0 files; waiting...\n");
    bg_cv_.Wait();
}
```

以上是`MakeRoomForWrite`函数的主要逻辑。其作用是管理memtable的空间，确保有足够的空间来进行新的写入操作。

OK, `MakeRoomForWrite`之后, 继续分析`DBImpl::Write`

```c++
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    // ...
    // 构造一个writer

    // ...
    // 将writer放入writers_中
    // 如果不是队列头节点, 则进入休眠
    // 醒来后发现活还在, 继续往下走, 干活

    // 检查是否有足够的空间来进行新的写入操作
    // 如果没有足够的空间, 则等待直到有足够的空间了, 再往下走

    uint64_t last_sequence = versions_->LastSequence();
    Writer* last_writer = &w;
    if (status.ok() &&
        updates != nullptr) {  // nullptr batch is for compactions
        WriteBatch* write_batch = BuildBatchGroup(&last_writer);
        WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
        last_sequence += WriteBatchInternal::Count(write_batch);

        // Add to log and apply to memtable.  We can release the lock
        // during this phase since &w is currently responsible for logging
        // and protects against concurrent loggers and concurrent writes
        // into mem_.
        {
            mutex_.Unlock();
            status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
            bool sync_error = false;
            if (status.ok() && options.sync) {
                status = logfile_->Sync();
                if (!status.ok()) {
                    sync_error = true;
                }
            }
            if (status.ok()) {
                status = WriteBatchInternal::InsertInto(write_batch, mem_);
            }
            mutex_.Lock();
            if (sync_error) {
                // The state of the log file is indeterminate: the log record we
                // just added may or may not show up when the DB is re-opened.
                // So we force the DB into a mode where all future writes fail.
                RecordBackgroundError(status);
            }
        }
        if (write_batch == tmp_batch_) tmp_batch_->Clear();

        versions_->SetLastSequence(last_sequence);
    }    

    // ...
}
```

先是获取一下当前的`Sequence`, 并且通过`BuildBatchGroup`构造一个新的`WriteBacch`.

```c++
uint64_t last_sequence = versions_->LastSequence();
Writer* last_writer = &w;

WriteBatch* write_batch = BuildBatchGroup(&last_writer);
WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
last_sequence += WriteBatchInternal::Count(write_batch);
```

前面我们提过, 只有`writers_`队列的头节点才有资格进行写入操作.
此处的`BuildBatchGroup`就是将`writers_`队列中的所有`writer`的`WriteBatch`合并成一个`WriteBatch`, 也就是队头的`writer`在执行写入的时候，会看一下队列里有没有其他的`writer`，如果有的话，就把这些`writer`的`WriteBatch`合并到自己的`WriteBatch`里，然后再执行写入操作。

这里的`last_writer`是一个指针，指向`writers_`队列的最后一个`writer`。`BuildBatchGroup`函数的作用是将`writers_`队列中的所有`writer`的`WriteBatch`合并成一个`WriteBatch`，并且将`last_writer`指向最后一个`writer`。这样做的目的是为了在后面的代码中，将`last_writer`的`status`设置为`status`，从而保证所有的`writer`都能够获取到正确的返回值。我们后面会再讲到`last_writer`的具体作用。

`WriteBatch`打包就绪后, 就可以把该`WriteBatch`写入`memtabl`了。

```c++
// mutex_是保护writers_的, memtable是线程安全的, 不需要加锁
// 在写memtable期间, 先把mutex_释放, 这样别的线程可以将writer_放入writers_中
mutex_.Unlock();

// 写memtable前, 需要先写WAL(Write Ahead Log)
// 如果中途crash, 可以通过WAL来恢复数据
status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
bool sync_error = false;
if (status.ok() && options.sync) {
    status = logfile_->Sync();
    if (!status.ok()) {
        sync_error = true;
    }
}

// 将write_batch写入memtable
// 我们后面会展开讲解WriteBatchInternal::InsertInto的实现
if (status.ok()) {
    status = WriteBatchInternal::InsertInto(write_batch, mem_);
}

// 此时已经写完memtable了, 需要把锁拿回来, 因为后面要访问writer_队列了
mutex_.Lock();
if (sync_error) {
    // The state of the log file is indeterminate: the log record we
    // just added may or may not show up when the DB is re-opened.
    // So we force the DB into a mode where all future writes fail.
    RecordBackgroundError(status);
}
```

我们来看下`WriteBatchInternal::InsertInto`的具体实现

```c++
Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* memtable) {
    MemTableInserter inserter;
    inserter.sequence_ = WriteBatchInternal::Sequence(b);
    inserter.mem_ = memtable;
    return b->Iterate(&inserter);
}
```

在`WriteBatchInternal::InsertInto`里, 构造了一个`MemTableInserter`， 然后调用`WriteBatch::Iterate`来遍历这个`MemTableInserter`, 我们再看下`WriteBatch::Iterate`的实现.
其实就是遍历`WriteBatch`中的每个`Record`, 然后调用`handler`的`Put`或`Delete`方法, 此处的`handler`就是`MemTableInserter`。

```c++
Status WriteBatch::Iterate(Handler* handler) const {

    // 把WriteBatch里的rep_转换成Slice,
    // 方便后续操作
    Slice input(rep_);
    if (input.size() < kHeader) {
        return Status::Corruption("malformed WriteBatch (too small)");
    }

    // 为什么要从input里把kHeader移除?
    // Slice(input)只是为了提取rep_里的key-values, 
    // kHeader包含了Sequence和Count, 这两部分信息还保留在WriteBatch的rep_里,
    // 如果需要还是可以拿得到
    input.remove_prefix(kHeader);

    Slice key, value;

    // 用found记录遍历input里处理过的Record数量,
    // 最后再对比看下found数是否与header里记录的Count相等.
    int found = 0;
    while (!input.empty()) {
        found++;

        // 取出当前key-value的type(tag)
        char tag = input[0];
        // 拿到了就可以移除了
        input.remove_prefix(1);
        switch (tag) {
            case kTypeValue:
                // type为kTypeValue, 说明是一个新增的key-value
                // 把key和value都取出来, 放进memtable里
                if (GetLengthPrefixedSlice(&input, &key) &&
                    GetLengthPrefixedSlice(&input, &value)) {
                    handler->Put(key, value);
                } else {
                    return Status::Corruption("bad WriteBatch Put");
                }
                break;
            case kTypeDeletion:
                // type为kTypeDeletion, 说明是一个删除的key
                // 不含value, 所以只取key, 放进memtable里
                if (GetLengthPrefixedSlice(&input, &key)) {
                    handler->Delete(key);
                } else {
                    return Status::Corruption("bad WriteBatch Delete");
                }
                break;
            default:
                return Status::Corruption("unknown WriteBatch tag");
        }
    }

    // 检查found数是否与header里记录的Count相等
    if (found != WriteBatchInternal::Count(this)) {
        return Status::Corruption("WriteBatch has wrong count");
    } else {
        return Status::OK();
    }
}
```

所以我们再看下`MemTableInserter::Put`和`MemTableInserter::Delete`的实现

```c++
class MemTableInserter : public WriteBatch::Handler {
   public:
    SequenceNumber sequence_;
    MemTable* mem_;

    void Put(const Slice& key, const Slice& value) override {
        mem_->Add(sequence_, kTypeValue, key, value);
        sequence_++;
    }
    void Delete(const Slice& key) override {
        mem_->Add(sequence_, kTypeDeletion, key, Slice());
        sequence_++;
    }
};
```

hhhh, 就是简单地调用了`MemTable::Add`方法, 把`Record`添加到`memtable`中.

不知道同学们看到这的时候，有没有和我一样疑惑，为啥需要一个`MemTableInserter`插一脚呢, 直接调用`memtable`的`Add`不就好啦？

其实用`MemTableInserter`封装一下会比较好, 我认为有两方面的好处。

1. 提高可读性。通过`Put`和`Delete`接口，我们可以很直白的知道是`Add`一条记录还是`Delete`一条记录。如果是直使用`Memtable::Add`，还得细看一下传入的是`kTypeValue`还是`kTypeDeletion`, 不如`Put`和`Delete`直白.
2. 可以添加一些额外的逻辑, 比如`sequence_++`.

OK, 到这里, `DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value)`的流程就已经走完了.

至于`Memtable`的写入部分, 见[LevelDB中Memtable的实现]()
