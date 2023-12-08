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

    // 向SST中添加一个键值对。
    // 要求键必须大于之前添加的任何键，需要以升序的方式添加键值对。
    void Add(const Slice& key, const Slice& value);

    // Flush()方法用于将缓冲区中的键值对立即写入到文件中。
    // 这个方法主要用于确保两个相邻的条目永远不会在同一个数据块中。
    // 大多数情况不需要使用这个方法。调用Flush()方法后，TableBuilder仍然可以继续添加新的键值对。
    void Flush();

    // 如果中途只要发生过一次错误，就会返回error。
    Status status() const;

    // Finish()方法用于完成SSTable的构建。
    // 调用这个方法后，TableBuilder将停止使用传递给构造函数的文件句柄，并将所有缓冲区中的键值对以及元数据写入到文件中。
    // 一旦调用了Finish()方法，就不能再向TableBuilder中添加新的键值对，也不能再次调用Finish()方法。
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
// 所有kv写入到SST文件中
Slice key;
for (; iter->Valid(); iter->Next()) {
    key = iter->key();
    builder->Add(key, iter->value());
}

// 完成SST的构建。
builder->Finish();
delete builder;
```

## SST的格式

TODO

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


