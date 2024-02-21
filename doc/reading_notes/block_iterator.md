# Block Iterator

`Block Iterator`用于遍历一个`SST`中的某个`Block`，`Block`的结构如下:

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

关于`Block`的结构详情可移步参考(大白话解析LevelDB: BlockBuilder)[https://blog.csdn.net/sinat_38293503/article/details/134997464#Block__153]。

## Iterator 接口

`Block::Iter`是`Iterator`接口的一种实现，所以我们先看下`Iterator`里都有哪些接口需要实现:

```cpp
class LEVELDB_EXPORT Iterator {
   public:
    Iterator();

    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    virtual ~Iterator();

    // 判断迭代器当前所在位置是否有效，如果有效，
    // 则可以通过 key() 和 value() 获取当前键值对。
    virtual bool Valid() const = 0;

    // 将当前位置移动到第一个 Key-Value 所在处。
    virtual void SeekToFirst() = 0;

    // 将当前位置移动到最后的 Key-Value 所在处。
    virtual void SeekToLast() = 0;

    // 将当前位置移动到第一个大于等于 target 的 Key-Value 所在处。
    virtual void Seek(const Slice& target) = 0;

    // 将当前位置移动到下一个 Key-Value 所在处。
    virtual void Next() = 0;

    // 将当前位置移动到上一个 Key-Value 所在处。
    virtual void Prev() = 0;

    // 返回当前位置的 Key。
    virtual Slice key() const = 0;

    // 返回当前位置的 Value。
    virtual Slice value() const = 0;

    // 返回迭代器的当前状态。
    // 如果状态不是 ok，则说明迭代器已经失效，不可使用了。
    virtual Status status() const = 0;

    // 用户可注册多个 CleanupFunction，当迭代器被销毁时，会按顺序调用这些 
    // CleanupFunction。
    // 需要注意的是，RegisterCleanup 这个方法不需要 Iterator 的子类实现，
    // Iterator 已经实现了，用户不需要重写。
    using CleanupFunction = void (*)(void* arg1, void* arg2);
    void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);
};
```

## Block Iterator 的实现

`Block::Iter`是`Iterator`接口的一种实现，它需要实现`Iterator`接口里的所有抽象方法。


### Block Iterator 的私有成员

在阅读`Block Iterator`的实现之前，我们先来看一下`Block Iterator`里各个私有成员的含义，为后续的阅读做好准备。

```cpp
class Block::Iter : public Iterator {
  private:
    // 构造 Iter 的时候传入一个 Comparator，决定了 Block 中的 key 的排序方式
    const Comparator* const comparator_;

    // data_ 指向存放 Block 数据的地方
    const char* const data_;       // underlying block contents

    // 通过 data_ + restarts_ 可得到重启点数组的起始位置
    uint32_t const restarts_;      // Offset of restart array (list of fixed32)

    // 重启点的数量
    uint32_t const num_restarts_;  // Number of uint32_t entries in restart array

    // current_ is offset in data_ of current entry.  >= restarts_ if !Valid
    //
    // 当前 Key-Value 在 data_ 中的偏移量
    uint32_t current_;

    // 当前处在哪个重启点，通过 restarts_[restart_index_] 可拿到当前重启点
    uint32_t restart_index_;  // Index of restart block in which current_ falls

    // 当前 Key-Value 中的 key
    std::string key_;

    // 当前 Key-Value 中的 value
    Slice value_;

    // 当前 Iterator 的状态
    Status status_;
}
```

### Block Iterator 的构造函数

从`SST`中解析出一个`Block`的以下 3 个信息后，就可以构造一个`Block Iterator`了:

- `data`指针
- 重启点数组在`data`中的偏移量
- 重启点的数量

```cpp
Iter(const Comparator* comparator, const char* data, uint32_t restarts, uint32_t num_restarts)
    : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts_),
        restart_index_(num_restarts_) {
    assert(num_restarts_ > 0);
}
```

### Block::Iter::Valid()

`current_`表示当前`Key-Value`在`data_`中的偏移量，`restarts_`表示重启点数组在`data_`中的偏移量，所以`current_`小于`restarts_`时，`Block::Iter`是有效的。

如果`current_`大于等于`restarts_`，`current_`就已经指向重启点的区域了，此时`Block::Iter`是无效的。

```cpp
bool Valid() const override { return current_ < restarts_; }
```

### Block::Iter::status()

没啥好说的，直接返回当前`Iterator`的状态。

```cpp
Status status() const override { return status_; }
```

### Block::Iter::key()

`Prev()`, `Next()`, `Seek()`方法会将`current_`指向一个有效的`Key-Value`，并且将该`Key-Value`的`key`和`value`分别存储到`key_`和`value_`中，所以`key()`方法直接返回`key_`即可。

```cpp
Slice key() const override {
    assert(Valid());
    return key_;
}
```

### Block::Iter::value()

`Prev()`, `Next()`, `Seek()`方法会将`current_`指向一个有效的`Key-Value`，并且将该`Key-Value`的`key`和`value`分别存储到`key_`和`value_`中，所以`key()`方法直接返回`key_`即可。

```cpp
Slice value() const override {
    assert(Valid());
    return value_;
}
```

### Block::Iter::Next()

将迭代器移动到下个`Key-Value`的位置。

```cpp
void Next() override {
    assert(Valid());
    // 将 current_ 移动到下一个 Key-Value 的位置，
    // 并且解析出 Key。
    ParseNextKey();
}
```

`Next()`的实现逻辑都甩给了`ParseNextKey()`.

`ParseNextKey()`的实现如下:

```cpp
bool ParseNextKey() {

    // 计算下一个 Key-Value 的偏移量
    current_ = NextEntryOffset();

    // 找到下一个 Key-Value 的首地址
    const char* p = data_ + current_;

    // limit 指向重启点数组的首地址
    const char* limit = data_ + restarts_;  // Restarts come right after data

    // 如果当前 Key-Value 的偏移量超过了重启点数组的首地址，
    // 表示已经没有 Next Key 了。
    if (p >= limit) {
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return false;
    }

    // 根据重启点来解析下一个 Key-Value
    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
        // 解析失败，将错误记录到 Iterator 的 status_ 里。
        CorruptionError();
        return false;
    } else {
        // 解析成功，此时 p 指向了 Key 的 non_shared 部分，
        // 将 shared 和 non_shared 部分拼接起来，得到当前 Key。
        key_.resize(shared);
        key_.append(p, non_shared);
        value_ = Slice(p + non_shared, value_length);

        // 更新 restart_index_，找到下一个重启点
        while (restart_index_ + 1 < num_restarts_ &&
                GetRestartPoint(restart_index_ + 1) < current_) {
            ++restart_index_;
        }
        return true;
    }
}
```

### Block::Iter::Prev()

`Prev()`与`Next()`实现不一样，不能简单的将`current`往上移一个位置。

见下图，假设`current_`指向`Key3`，上一个`Key`是`Key2`。`Key2`和`Key3`里都没有存储完整的`Key`，`Key3`需要依赖`Key2`来解析出完整的`Key3`，而`Key2`需要依赖`Key1`来解析出完整的`Key2`，以此类推，`Key1`需要依赖`Key0`来解析出完整的`Key1`，而`Key0`位于重启点，是没有依赖的，自己本身就存储了完整的`Key`。

所以要找到`Key2`，我们需要先找到`Key3`之前最近的一个重启点，也就是`RestartPoint0`。

然后将`current_`移动到该重启点的位置，再不断的向后边移动`current_`，边解析出当前`Key`，直到`current_`指向`Key3`的前一个`Key`，也就是`Key2`。

```plaintext
                              current_
                                 +
                                 |
                                 v

  +--------+--------+--------+--------+--------+--------+
  |  Key0  |  Key1  |  Key2  |  Key3  |  Key4  |  Key5  |
  +--------+--------+--------+--------+--------+--------+

      ^                                   ^
      |                                   |
      +                                   +
RestartPoint0                       RestartPoint1

```

```cpp
void Prev() override {
    assert(Valid());

    // 把 restart_index_ 移动到 current_ 之前最近的一个重启点。
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
        if (restart_index_ == 0) {
            // 前面没有 Key 了
            current_ = restarts_;
            restart_index_ = num_restarts_;
            return;
        }
        restart_index_--;
    }

    // 把 current_ 移动到重启点的位置，
    SeekToRestartPoint(restart_index_);
    do {
        // 将 current_ 不断向后移动，一直移动到原先 current_ 的前一个位置。
    } while (ParseNextKey() && NextEntryOffset() < original);
}
```

### Block::Iter::Seek(const Slice& target)

理解`Prev()`的实现后，`Seek()`的实现就比较容易理解了。我们不能直接去找`target`的位置，而是需要先找到`target`的那个重启点。然后再从这个重启点开始，一步步向右移动`current_`，直到找到第一个大于等于`target`的`Key`。

而找重启点的过程，是一个二分查找的过程。

```cpp
void Seek(const Slice& target) override {
    // 理解 Prev() 的实现后，Seek() 的实现就比较容易理解了。
    // 我们不能直接去找 target 的位置，而是需要先找到 target 
    // 的那个重启点。用二分的方式查找这个重启点。
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    int current_key_compare = 0;

    if (Valid()) {
        // 如果当前 Iterator 已经指向某个有效的 Key 了，我们就可以利用这个 Key 来把
        // 二分查找 target 重启点的范围缩小一些。
        current_key_compare = Compare(key_, target);
        if (current_key_compare < 0) {
            // 如果当前 Key 比 target 小，那么 target 重启点的位置一定在当前重启点的
            // 右边，包括当前重启点。
            left = restart_index_;
        } else if (current_key_compare > 0) {
            // 如果当前 Key 比 target 大，那么 target 重启点的位置一定在当前重启点的
            // 左边，包括当前重启点。
            right = restart_index_;
        } else {
            // 如果当前 Key 已经是 target 了，那么就不需要再做其他操作了。
            return;
        }
    }

    // 二分查找 target 的重启点。
    while (left < right) {
        // 找到 left 和 right 中间的那个重启点，看这个重启点的 Key 和 target 的大小关系。
        //   - 如果 mid 重启点的 Key 比 target 小，那么 target 重启点的位置一定在 mid 
        //     的右边，需要吧 left 移动到 mid 的位置。
        //   - 如果 mid 重启点的 Key 比 target 大，那么 target 重启点的位置一定在 mid
        //     的左边，需要把 right 移动到 mid 的位置。    
        uint32_t mid = (left + right + 1) / 2;
        uint32_t region_offset = GetRestartPoint(mid);
        uint32_t shared, non_shared, value_length;
        const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                                            &non_shared, &value_length);
        if (key_ptr == nullptr || (shared != 0)) {
            CorruptionError();
            return;
        }
        Slice mid_key(key_ptr, non_shared);
        if (Compare(mid_key, target) < 0) {
            left = mid;
        } else {
            right = mid - 1;
        }
    }

    // 找到 target 的重启点后，一步步向右移动 current_，直到找到第一个大于等于
    // target 的 Key。
    assert(current_key_compare == 0 || Valid());
    bool skip_seek = left == restart_index_ && current_key_compare < 0;
    if (!skip_seek) {
        SeekToRestartPoint(left);
    }
    while (true) {
        if (!ParseNextKey()) {
            return;
        }
        if (Compare(key_, target) >= 0) {
            return;
        }
    }
}
```

### Block::Iter::SeekToFist()

```cpp
void SeekToFirst() override {
    // 移动到一个重启点。
    SeekToRestartPoint(0);
    // 解析出第一个 Key。
    ParseNextKey();
}
```

### Block::Iter::SeekToLast()

```cpp
void SeekToLast() override {
    // 移动到最后一个重启点。
    SeekToRestartPoint(num_restarts_ - 1);
    // 不断向后移动 current_，直到最后一个 Key。
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
    }
}
```