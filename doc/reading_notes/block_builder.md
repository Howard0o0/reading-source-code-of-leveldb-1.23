# BlockBuilder

BlockBuilder 是一个 Block 的构建器，它会将 Block 的 Key-Value 以及重启点信息编码到一个`char* buffer`里。

什么是重启点，我们后面会讲到。

BlockBuilder 的用法如下：

通过`Add(const Slice& key, const Slice& value)`方法，我们可以将需要 Key-Value 添加到 BlockBuilder 的缓冲区里，最后再通过`Finish()`方法，获取到 Block 的内容。

```c++
class BlockBuilder {
   public:
    explicit BlockBuilder(const Options* options);

    BlockBuilder(const BlockBuilder&) = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;

    // 恢复到一个空白 Block 的状态
    void Reset();

    // 往 block buffer 里添加一对 Key-Value
    void Add(const Slice& key, const Slice& value);

    // 将重启点信息也压入到 Block 缓冲区里，结束该 Block 的构建，
    // 然后返回 buffer_。
    Slice Finish();

    // 返回 Block 的预估大小。
    // 准确的说，是 Block 的原始大小，但是 Block 写入
    // SST 前会进行压缩，所以此处只能返回一个预估大小。
    size_t CurrentSizeEstimate() const;

    // 判断 buffer_ 是否为空
    bool empty() const { return buffer_.empty(); }
};
```

## BlockBuilder 的代码实现

### BlockBuilder::Add(const Slice& key, const Slice& value)

通过`BlockBuilder::Add(const Slice& key, const Slice& value)`方法，我们可以将 Key-Value 添加到 BlockBuilder 的缓冲区里，并且会对 Key 进行前缀压缩，每隔 N 个 Key 会添加一个重启点。

前缀压缩就是将 Key 的公共前缀去掉，只存储后面不同的部分。比如，Block 里最后一个 Key 是 "hello"，此时再插入一个 Key "hello_world"，那么该 Key 存储在 Block 里的内容就是 "_world"。

重启点的意思是，从该重启点开始，第一个 Key 不再进行前缀压缩，而是直接存储整个 Key。

举个例子，假设重启间隔 N = 3，那么 Block 里的 Key-Value 可能是这样的：

```c++
+-----+ +-----+ +-----+ +-----+ +-----+ +-----+
| kv1 | | kv2 | | kv3 | | kv4 | | kv5 | | kv6 |
+-----+ +-----+ +-----+ +-----+ +-----+ +-----+
                       ^
                       |
                       |
                       +
                 restart point
```

其中，k1 和 k4 是完整存储的，k2、k3、k5、k6 只存储前缀压缩后的结果。

那为什么需要重启点呢？Block 里所有 Key 都进行前缀压缩不行吗？

如果没有重启点的话，Block 里只有第一个 Key 是完整存储的，后面的 Key 存储的只是前缀压缩后的结果。这样当我们进行范围查找的时候，Block 里有 100 个 Key，而我们只需要最末尾的 10 个 Key，但也需要将前面所有的 Key 都解压出来，才能拿到最后的 10 个 Key的内容。 如果有了重启点，并且刚好重启间隔是 10，那么我们只需要解压最后一个重启点之后的 10 个 Key，就能拿到最后的 10 个 Key 的内容了。

OK，现在我们了解前缀压缩与重启点后，就阔以往下看`BlockBuilder::Add`的实现了:

- 首先计算当前 Key 和上一个 Key 的共享前缀长度，记录到变量`shared`里。
- 若满足重建重启点的条件，则将`shared`置为 0，表示当前 Key 不需要压缩存储。并且将重启点的位置记录到`restarts_`数组里。
- 将`shared(共享前缀长度)`、`non_shared(非共享长度)`、`value_size(value的长度)`按顺序写入`buffer_`里。
- 将 `Key` 的非共享部分写入`buffer_`里。如果`shared`为 0，则非共享部分就是整个`Key`。
- 将`Value`写入`buffer_`里。

```c++
void BlockBuilder::Add(const Slice& key, const Slice& value) {
    // 获取上一个添加的 Key: last_key_ 
    Slice last_key_piece(last_key_);
    assert(!finished_);
    assert(counter_ <= options_->block_restart_interval);
    // 如果 buffer_ 非空，检查 key 是否大于最后一个被添加到 Block 中的 Key
    assert(buffer_.empty()  // No values yet?
           || options_->comparator->Compare(key, last_key_piece) > 0);

    // 初始化一个变量shared，
    // 用于记录新添加的键和上一次添加的键的共享前缀的长度。
    size_t shared = 0;

    // 如果 counter_ < block_restart_interval的话，
    // 说明还没有到重启点，计算当前键和上一次添加的键的共享前缀的长度。
    if (counter_ < options_->block_restart_interval) {
        // 计算当前键和上一次添加的键的共享前缀的长度 shared
        const size_t min_length = std::min(last_key_piece.size(), key.size());
        while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
            shared++;
        }
    } else {
        // counter_ == block_restart_interval，
        // 满足新建重启点的条件了，重置 counter_ 为 0，
        // 并且将当前的偏移量压入 restarts_ 数组中，记录当前重启点的位置。
        // 重启点的第一个 Key 不需要进行前缀压缩，
        // 所以此时保持 shared 的值为 0 即可。
        restarts_.push_back(buffer_.size());
        counter_ = 0;
    }

    // 计算 key 和 last_key_ 的非共享长度
    const size_t non_shared = key.size() - shared;

    // 使用变长编码，将 "<shared><non_shared><value_size>" 写入 buffer_
    PutVarint32(&buffer_, shared);
    PutVarint32(&buffer_, non_shared);
    PutVarint32(&buffer_, value.size());

    // 将 Key 非共享内容压入 buffer_ 
    buffer_.append(key.data() + shared, non_shared);
    // 将完整的 Value 压入 buffer_
    buffer_.append(value.data(), value.size());

    // 更新 last_key_
    last_key_.resize(shared);
    last_key_.append(key.data() + shared, non_shared);
    assert(Slice(last_key_) == key);
    counter_++;
}
```

### BlockBuilder::Finish()

在`BlockBuilder::Finish`之前，已经通过`BlockBuilder::Add`将 Key-Value 添加到了`buffer_`里。

`BlockBuilder::Finish`会在`buffer_`的末尾添加重启点信息，并且将`buffer_`返回。

```c++
Slice BlockBuilder::Finish() {
    // 先把 restarts_ 中的所有重启点位置压入 buffer_ 中
    for (size_t i = 0; i < restarts_.size(); i++) {
        PutFixed32(&buffer_, restarts_[i]);
    }

    // 再将重启点的数量压入到 buffer_ 中
    PutFixed32(&buffer_, restarts_.size());

    // 设置结束标志位
    finished_ = true;
    
    // 返回完整的 buffer_ 内容
    return Slice(buffer_);
}
```

## Block 的内容格式

现在我们可以结合`BlockBuilder::Finish`和`BlockBuilder::Add`的实现，来看看最终的 Block 的内容格式是什么样的。

```plaintext
+----------------+
|    Key1:Value1 |
+----------------+
|    Key2:Value2 |
+----------------+
|       ...      |
+----------------+
|    KeyN:ValueN |
+----------------+
| Restart Point1 |
+----------------+
| Restart Point2 |
+----------------+
|       ...      |
+----------------+
| Restart PointM |
+----------------+
| Num of Restarts|
+----------------+
```

其中，每条`Key:Value`的格式如下:

```plaintext
+------------+----------------+-----------+---------------------+------------+
| shared_len | not_shared_len | value_len | not_shared_key_data | value_data |
+------------+----------------+-----------+---------------------+------------+
```




