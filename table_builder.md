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

    void Flush();

    // 如果中途只要发生过一次错误，就会返回error。
    Status status() const;

    // 完成SST的构建，
    // 将缓冲区的内容写入到SST文件中。
    Status Finish();

    // Indicate that the contents of this builder should be abandoned.  Stops
    // using the file passed to the constructor after this function returns.
    // If the caller is not going to call Finish(), it must call Abandon()
    // before destroying this builder.
    // REQUIRES: Finish(), Abandon() have not been called
    /* 放弃 Table 的构建 */
    void Abandon();

    // Number of calls to Add() so far.
    /* 一共添加了多少 Key-Value 对 */
    uint64_t NumEntries() const;

    // Size of the file generated so far.  If invoked after a successful
    // Finish() call, returns the size of the final generated file.
    uint64_t FileSize() const;

   private:
    bool ok() const { return status().ok(); }
    /* 序列化需要写入的 Data Block */
    void WriteBlock(BlockBuilder* block, BlockHandle* handle);
    /* 将压缩后的数据写入文件中 */
    void WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

    /* Rep 的作用就是隐藏具体实现 */
    struct Rep;
    Rep* rep_;
};
```