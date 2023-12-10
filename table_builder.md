# TableBuilder

LevelDB将`MemTable`生成`SST`的相关操作封装成了`TableBuilder`类，`TableBuilder`类的定义如下：

```cpp
class LEVELDB_EXPORT TableBuilder {
   public:
    // Options对象包含了一些配置选项，
    // WritableFile对象是用于写入SSTable的文件。
    TableBuilder(const Options& options, WritableFile* file);

    TableBuilder(const TableBuilder&) = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    ~TableBuilder();

    // 改变TableBuilder的配置选项。注意，只有部分配置选项可以在构造后改变。
    Status ChangeOptions(const Options& options);

    // 向`Data Block`中添加`Key-Value`，并同步更新其他`Block`，
    // 如`Filter Block`、`Index Block`。
    // 只是添加到`Block`的缓冲区，还没有写入到文件。
    // 最后再通过调用`Finish()`方法将各个`Block`缓冲区中的内容写入到文件中。
    void Add(const Slice& key, const Slice& value);

    // 该函数名有点容易让人误解成刷盘，其实不是的。
    // Flush只是结束当前`Data Block`的构建，并开辟一个新的`Data Block`。
    // 当前`Data Block`满后，会调用`Flush()`。
    void Flush();

    // 如果中途只要发生过一次错误，就会返回error。
    Status status() const;

    // Finish()方法会将各个Block的内容写入到文件中，并且在文件的尾部添加一个`Footer`。
    // 结束该`SST`文件的构建。
    Status Finish();

    // 放弃SSTable的构建。在调用此方法后，之前添加到TableBuilder中的所有键值对都将被丢弃。
    void Abandon();

    // 一共添加了多少 Key-Value 对
    uint64_t NumEntries() const;

    // SST的当前大小。
    uint64_t FileSize() const;

   private:
    bool ok() const { return status().ok(); }

    struct Rep;
    Rep* rep_;
};
```

其中`Rep`是"Representation"的缩写，是一种常用的C++手法，用于实现类的 Pimpl（Pointer to Implementation）模式。具体移步参考[大白话解析LevelDB：Pimpl模式](https://blog.csdn.net/sinat_38293503/article/details/134830981)。

## `TableBuilder`的使用姿势

我们来简单看下`TableBuilder`的使用姿势：

```c++
// 创建SST文件
WritableFile* file;
s = env->NewWritableFile(fname, &file);
if (!s.ok()) {
    return s;
}

// 创建一个TableBuilder对象，
// 用于将MemTable中的数据写入到SST文件中
TableBuilder* builder = new TableBuilder(options, file);

// 通过TableBuilder对象将
// 所有kv先写入到各个`Block`缓冲区里。
Slice key;
for (; iter->Valid(); iter->Next()) {
    key = iter->key();
    builder->Add(key, iter->value());
}

// 将各个`Block`缓冲区里的内容写入到文件中，
// 该`SST`文件构建完成。
builder->Finish();
delete builder;
```

## SST的格式

移步参考[大白话解析LevelDB：数据格式(SST)]https://blog.csdn.net/sinat_38293503/article/details/134739340?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22134739340%22%2C%22source%22%3A%22sinat_38293503%22%7D#SST_92

## `TableBuilder`的构造函数

```c++
TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {
    // 如果options中指定了filter polacy配置选项，
    // rep_就会按照filter polac初始化一个新的filter block,
    // 此时rep_->filter_block就不为nullptr了。
    if (rep_->filter_block != nullptr) {
        // 初始化一个新的filter block.
        rep_->filter_block->StartBlock(0);
    }
}
```

构造`TableBuilder`的时候，会把LevelDB的`Option`与`SST`的文件句柄`file`传入到`TableBuilder`对象中。

`Option`里可以配置`filter policy`，比如`BloomFilterPolicy`，给每个`SST`都配置一个`Bloom Filter`，用于加速`key`的查找。不了解`Bloom Filter`的同学可以自行搜索一下哈。 

如果配置了`filter policy`，`TableBuilder`会初始化一个`filter block`，并且调用`filter block`的`StartBlock()`方法，构建一个新的`filter block`。

## TableBuilder::Add(const Slice& key, const Slice& value)

此处的`key`指的是`Internal Key`，即`User Key + Sequence Number | Value Type`。

`TableBuilder::Add(const Slice& key, const Slice& value)`的职责是将`key`和`value`添加到`SST`中，具体来讲，是更新`SST`中的`Data Block`，`Index Block`，`Filter Block`。

`SST`中`Data Block`的大小是有上限的。往当前`Data Block`插入一对`Key-Value`后，如果当前的`Data Block`大小超过上限了，就需要结束当前`Data Block`的构建，开始构建下一个`Data Block`。

并且结束当前`Data Block`的构建后，还需要将当前`Data Block`的`Index`添加到`Index Block`里。

每添加一个`Key-Value`，都要把这个`Key`添加到`Filter Block`里，如果`Option`里配置了`Filter Policy`的话。

```c++
void TableBuilder::Add(const Slice& key, const Slice& value) {
    Rep* r = rep_;
    assert(!r->closed); // 检查当前 Build 过程是否已经结束
    if (!ok()) return;
    if (r->num_entries > 0) {
        // 检查当前 key 是否大于 last_key
        assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
    }

    // 当r->pending_index_entry为true时，
    // 表示上一个`Data Block`已经构建完了，需要把它(上一个`Data Block`)的`Index`添加到`Index Block`里。
    if (r->pending_index_entry) {
        assert(r->data_block.empty());

        // `FindShortestSeparator`用于计算上一个`Key`和当前`Key`的分隔符，
        // 即`Last Key` <= `Seperator` < `Current Key`，用于上一个`Data Block`的最大Key。
        // 我们先说下分隔符`Separator`的作用，再说为什么不直接使用上一个`Data Block`的最后一个Key作为分隔符。
        // 当LevelDB在查找某个`Key`的时候，会先定位到是哪个`SST`，然后再到`SST`内部查找。
        // 我们可以直接对`SST`二分查找，但还可以更快，就是将`SST`里的`Key-Value`按照`Data Block`分组。
        // 每个`Data Block`的大小固定，先定位目标`Key`在哪个`Data Block`，然后再到该`Data Block`内部查找。
        // SST里有一个`Index Block`，是用来存放`Data Block`的索引的，它长这样：
        // 
        // +------------------------+
        // | Key1 | Offset1 | Size1 |
        // +------------------------+
        // | Key2 | Offset2 | Size2 |
        // +------------------------+
        // | Key3 | Offset3 | Size3 |
        // +------------------------+
        // |          ....          |
        // +------------------------+
        //
        // `Index Block`里一行{ Key-Offset-Size }就代表了一个`Data Block`。
        // 其中，`Key1`代表了`Data Block 1`里`Key`的最大值。
        // `Offset`代表了`Data Block 1`在SST里的偏移量，`Size`表示`Data Block 1`的大小。
        // (Key1, Key2]就代表了`Data Block 2`里`Key`的取值范围。
        //
        // `FindShortestSeparator`就是用来生成`Data Block`对应于`Index Block`里的`Key`的。
        // 
        // 那为什么不直接用`Data Block 2`里最后一个`Key`作为`Index Block`里的`Key2`呢？
        // 还要通过`FindShortestSeparator`生成。
        // 为了压缩`Index Block`的空间，用一个最短的`Seperator`表示`Data Block`的`Upper Bound`。
 
        r->options.comparator->FindShortestSeparator(&r->last_key, key);

        // `handle_encoding`里存放的就是`Data Block`的`Offset`和`Size`。
        std::string handle_encoding;
        r->pending_handle.EncodeTo(&handle_encoding);

        // 将上一个Data Block的`Key-Offset-Size`追加到`Index Block`里。
        r->index_block.Add(r->last_key, Slice(handle_encoding));

        // 上一个`Data Block`的`Index`已经添加到`Index Block`里了，
        // 把`pending_index_entry`置为false，表示当前没有待添加`Index`的`Data Block`。
        r->pending_index_entry = false;
    }

    // 如果配置了`Filter`，就将`Key`添加到`Filter Block`里。
    // 此处的`Key`为`Internal Key`，即`User Key + Sequence Number | Value Type`。
    // 为什么添加到`Filter Block`里的是`Internal Key`而不是`User Key`呢？
    // 为了支持多版本控制。
    // 举个例子，当用户创建了一个快照，然后把某个`Key`删除了，此时从快照里查找这个`Key`，
    // 如果`Filter Block`里存放的是`User Key`，就没法去查找快照版本的`Key`，只能
    // 查找最新版本的，结果就是`Key`已经不存在了。
    if (r->filter_block != nullptr) {
        r->filter_block->AddKey(key);
    }

    // 更新last_key，用于下一个`Data Block`的`FindShortestSeparator`。
    r->last_key.assign(key.data(), key.size());

    // 更新`SST`里的`Key-Value`数量。
    r->num_entries++;
    
    // 将`Key-Value`添加到`Data Block`里。
    r->data_block.Add(key, value);

    // `options.block_size`为`Data Block`的大小的上限，
    // 默认为4KB，当`Data Block`的大小超过这个值时，就需要结束当前
    // `Data Block`的构建，开始构建下一个`Data Block`。
    const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
    if (estimated_block_size >= r->options.block_size) {
        // `TableBuilder::Flush()`会结束当前`Data Block`的构建，生成`Block Handle`，
        // 并且把`pending_index_entry`置为`true`，表示当前有一个待添加`Index`的`Data Block`。
        // 等到添加下一个`Key`时，就会把这个满的`Data Block`的`Index`添加到`Index Block`里。
        Flush();
    }
}
```

`r->options.comparator->FindShortestSeparator(&r->last_key, key)`的实现详情请移步[TODO](TODO)。
