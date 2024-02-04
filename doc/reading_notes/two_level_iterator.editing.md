# TwoLevelIterator

`TwoLevelIterator`其实就是用于遍历`SST`里所有`Key-Value`的`Iterator`。

那为什么要叫做`TwoLevelIterator`而不叫`SSTIterator`呢？`TwoLevel`指的是哪两个`Level`呢？

`TwoLevel`不是指同时遍历两层`Level`，而是指`SST`里的`Index Block`和`Data Block`两层。

`SST`的结构如下:

```plaintext
+---------------------+
|   Data Block 1      |
+---------------------+
|   Data Block 2      |
+---------------------+
|        ...          |
+---------------------+
|   Data Block N      |
+---------------------+
|   Meta Block 1      |
+---------------------+
|        ...          |
+---------------------+
|   Meta Block K      |
+---------------------+
| Metaindex Block     |
+---------------------+
|   Index Block       |
+---------------------+
|      Footer         |
+---------------------+
```

`SST`将`Key-Value`分散在多个`Data Block`里，`Index Block`里存储每个`Data Block`的`Key`范围和在`SST`文件中的偏移量。

`Index Block`的内容如下:

```plaintext
+--------------------------------------------------+
| Key1 | Block Handle1 (指向第一个 Data Block 的信息) |
+--------------------------------------------------+
| Key2 | Block Handle2 (指向第二个 Data Block 的信息) |
+--------------------------------------------------+
| Key3 | Block Handle3                             |
+--------------------------------------------------+
| ...............                                  |
+--------------------------------------------------+
| KeyN | Block HandleN                             |
+--------------------------------------------------+
```

`Block Handle1`里包含了`Data Block 1`在`SST`文件中的偏移量和大小。

想象一下我们现在要查找一个`Key-X`，需要先到`Index Block`中通过二分查找的方式找到对应的`Block Handle`，然后通过这个`Block Handle`找到对应的`Data Block`，最后再到`Data Block`中查找这个`Key-X`。

所以每次查找我们都要先到`Index Block`中查找，然后再到`Data Block`中查找，这就是`TwoLevelIterator`的`TwoLevel`。

## Iterator 接口

`TwoLevelIterator`是`Iterator`接口的一种实现，所以我们先看下`Iterator`里都有哪些接口需要实现:

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

## TwoLevelIterator 的实现

`TwoLevelIterator`是`Iterator`接口的一种实现，它需要实现`Iterator`接口里的所有抽象方法。

### TwoLevelIterator 的构造函数

```cpp
TwoLevelIterator::TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                                   const ReadOptions& options)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(nullptr) {}
```

`TwoLevelIterator`的构造函数挺简单的，对一些成员变量进行了初始化。

主要是`block_function_`这个参数，它是一个函数指针，用于获取`Data Block`的`Iterator`。

我们来看下`BlockFunction block_function`的定义:

```cpp
typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);
```

`BlockFunction`是一个函数指针，指向一个用于创建`Data Block`的`Iterator`的函数。

当`TwoLevelIterator`需要创建一个新的`Data Block`的`Iterator`时，它会调用`block_function_`。例如，当`index_iter_`移动到下一处了，`TwoLevelIterator`需要获取对应的`Data Block`更新`data_ter`，这时，它会调用`block_function_`。

我们来看下`TwoLevelIterator`是如何使用`block_function_`的。

#### TwoLevelIterator::InitDataBlock

```cpp
void TwoLevelIterator::InitDataBlock() {
    if (!index_iter_.Valid()) {
        // 如果 index_iter_ 无效，那对应的 data_iter_ 也是无效的。
        SetDataIterator(nullptr);
    } else {
        // 从 index_iter 中取出对应的 data_block 的 handle。
        Slice handle = index_iter_.value();
        if (data_iter_.iter() != nullptr && handle.compare(data_block_handle_) == 0) {
            // 与 handle 对应的 data_iter_ 已经构建好了，
            // 不需要重复构建。
        } else {
            // 通过 block_function_ 构建 与 handle 对应的 data_iter_。
            Iterator* iter = (*block_function_)(arg_, options_, handle);
            data_block_handle_.assign(handle.data(), handle.size());
            SetDataIterator(iter);
        }
    }
}
```

`TwoLevelIterator::InitDataBlock`中，会从`index_iter_`中取出对应的`data_block`的`handle`，然后通过`block_function_`构建与`handle`对应的`data_iter_`。

`SetDataIterator(iter)`就是用于设置`data_iter_`的。

```cpp
void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
    if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
    data_iter_.Set(data_iter);
}
```


### TwoLevelIterator::Seek(const Slice& target)

```cpp
void TwoLevelIterator::Seek(const Slice& target) {
    // index_iter_ 对应着 index_block，到 index_block 中
    // 查找 target 属于哪个 data_block。
    index_iter_.Seek(target);
    // 如果找到了这个 data_block，就从 SST 中加载这个 data_block。
    InitDataBlock();
    // data_iter_.iter() != nullptr 从 SST 中加载到 target
    // 所属的 data_block了。
    if (data_iter_.iter() != nullptr) data_iter_.Seek(target);
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksForward();
}
```

`target`是我们要查找的`Key`，它存储在`SST`的`Data Block`中。

要找到这个`Data Block`，需要先通过`index_iter_`在`Index Block`里查找`target`属于哪个`Data Block`，找到这个`Data Block`的`data_iter`，然后通过`data_iter`在`Data Block`中查找`target`。

现在我们应该看出为什么要叫做`TwoLevelIterator`了，指的就是`index_iter`和`data_iter`这两个`Iterator`。

`index_iter.Seek(target)`的实现可移步参考[TODO]()。

`data_iter.Seek(target)`的实现可移步参考[TODO]()。

`SkipEmptyDataBlocksForward()`的实现如下：

```cpp
void TwoLevelIterator::SkipEmptyDataBlocksForward() {
    while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
        // index_iter_ 无效，表示 index_block 已经遍历完了。
        // 此时将 data_iter_ 置为 null 表示 data_iter_ 已经
        // 到末尾了。
        if (!index_iter_.Valid()) {
            SetDataIterator(nullptr);
            return;
        }
        // index_iter_ 里还有东西，继续往后走一位。
        index_iter_.Next();
        // 加载与当前 index_iter_ 对应的 Data Block。
        InitDataBlock();
        // 如果 data_iter_ 有效，就将 data_iter_ 移到第一个有效的位置。
        if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
    }
}
```

如果`index_iter_`对应的`data_iter_`里没有有效数据，就将`index_iter_`往后移一位, 加载下一个`data_block`。一直到找到有数据的`data_block`为止。

### TwoLevelIterator::SeekToFirst

```cpp
void TwoLevelIterator::SeekToFirst() {
    // 到 index_block 中查找第一个 data_block 是哪个。
    index_iter_.SeekToFirst();
    // 从 SST 中加载这个 data_block。
    InitDataBlock();
    // data_iter_.iter() != nullptr 从 SST 中加载到第一个有效的
    // data_block 了。
    if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksForward();
}
```

`TwoLevelIterator::SeekToFirst`与`TwoLevelIterator::Seek`的逻辑大体一样，先通过`index_iter`找到对应的`data_iter`，然后再通过`data_iter`定位`data_block`里的第一对`Key-Value`。

不过有同学可能会对`if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst()`里的`if (data_iter_.iter() != nullptr)`判断有疑惑，难道`SST`里全是无效的`Data Block`？

假设我们不停的对`LevelDB`做删除操作，`db->Delete(leveldb::WriteOptions(), key)`，那某个`SST`里的`Key`就全是`Delete`类型的，这时候我们对这个`SST`做`SeekToFirst`操作，就找不到任何有效的`Data Block`。

### TwoLevelIterator::SeekToLast

`TwoLevelIterator::SeekToLast`的逻辑和`TwoLevelIterator::SeekToFirst`一样，就不再赘述啦。

```cpp
void TwoLevelIterator::SeekToLast() {
    // 到 index_block 中查找最后一个 data_block 是哪个。
    index_iter_.SeekToLast();
    // 从 SST 中加载这个 data_block。
    InitDataBlock();
    // data_iter_.iter() != nullptr 从 SST 中加载到第一个有效的
    // data_block 了。
    if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksBackward();
}
```

### TwoLevelIterator::Next

```cpp
void TwoLevelIterator::Next() {
    assert(Valid());
    // 将 data_iter_ 往后移一位即可。
    data_iter_.Next();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksForward();
}
```

`TwoLevelIterator::Next`就不需要用到`index_iter_`了，将`data_iter_`按顺序往后移一位即可。

### TwoLevelIterator::Prev

```cpp
void TwoLevelIterator::Prev() {
    assert(Valid());
    // 将 data_iter_ 往前移一位即可。
    data_iter_.Prev();
    // 跳过不包含数据的 data_block。
    SkipEmptyDataBlocksBackward();
}
```

与`TwoLevelIterator::Next`同理，不需要用到`index_iter_`，将`data_iter_`按顺序往前移一位即可。

### TwoLevelIterator::key

```cpp
Slice key() const override {
    assert(Valid());
    // 取出 data_iter_ 当前位置的 key。
    return data_iter_.key();
}
```

取出`data_iter_`当前位置的`key`即可。

### TwoLevelIterator::value

```cpp
Slice value() const override {
    assert(Valid());
    // 取出 data_iter_ 当前位置的 value。
    return data_iter_.value();
}
```

取出`data_iter_`当前位置的`value`即可。

### TwoLevelIterator::status

```cpp
Status status() const override {
    if (!index_iter_.status().ok()) {
        // 先检查 index_iter_ 是否有异常
        return index_iter_.status();
    } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
        // 再看 data_iter_ 是否有异常
        return data_iter_.status();
    } else {
        // index_iter_ 和 data_iter_ 都没异常，
        // 返回 TwoLevelIterator 自己的状态信息。
        return status_;
    }
}
```

先检查`index_iter`和`data_iter_`是否有异常，如果有的话，先返回它们的异常信息。

如果`index_iter`和`data_iter_`都没有异常，就返回`TwoLevelIterator`自己的状态信息。

