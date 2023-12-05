# Memtable

LevelDB在写入一个`key-value`时, 不会直接将该`key-value`落盘。而是先将该`key-value`写入内存中的`memtable`, 当`memtable`的大小达到一定阈值时, 再将`memtable`整个落盘成一个`sstable`文件。

我们先来看下`memtable`的接口, 都提供了哪些功能:

```cpp
class MemTable {
   public:
    
    explicit MemTable(const InternalKeyComparator& comparator);

    // 不允许拷贝和赋值
    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    // Increase reference count.
    void Ref() { ++refs_; }

    // Drop reference count.  Delete if no more references exist.
    void Unref() {
        --refs_;
        assert(refs_ >= 0);
        if (refs_ <= 0) {
            delete this;
        }
    }

    void Add(SequenceNumber seq, ValueType type, const Slice& key,
             const Slice& value);

    bool Get(const LookupKey& key, std::string* value, Status* s);


    size_t ApproximateMemoryUsage();

    Iterator* NewIterator();

   private:
    // ...
};
```

`memtable`有两类接口

- 数据读写接口
  - `Add`: 添加一个`key-value`到`memtable`
  - `Get`: 从`memtable`中查找一个`key`对应的`value`
  - `NewIterator`: 生成一个`memtable`的迭代器
  - `ApproximateMemoryUsage`: 获取`memtable`的内存占用
- 内存管理接口
  - `Ref`: 增加`memtable`的引用计数
  - `Unref`: 减少`memtable`的引用计数, 当引用计数为0时, 释放`memtable`的内存

先从简单的看起, 来看下Memtable的构造函数.

## MemTable的构造函数

```c++
explicit MemTable(const InternalKeyComparator& comparator) \
    : comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}
```

`MemTable`是一个有序集合, 数据始终是按照`key`的顺序排列的, 所以`MemTable`的构造函数中, 传入了一个`InternalKeyComparator`对象, 用于比较两个`key`的大小。
至于为什么是`InternalKeyComparator`, 而不是`KeyComparator`, 是因为`Memtable`需要确保每个`Key`的唯一性, 即使用户`Add`了多个相同`Key`, 但也会结合`SequenceNumber`和`ValueType`来确保每个`Key`的唯一性。

```
InternalKey = Sequence + Type + UserKey
```

除了`comparator_`, 构造函数中还会初始化`refs_`和`table_`.
`refs_`代表该`memtable`的引用次数, 当`refs_`为0时, 该`memtable`将会被销毁。
`table_`是一个`SkipList`, 用于实际存储`memtable`中的`key-value`数据。
之所以为什么要将`SkipList`封装在`memtable`中, 而不是直接使用`SkipList`, 是为了灵活性. 抽象出一个`Memtable`, `SkipList`是其中一种实现. 用户如果有特殊需求, 可以将`SkipList`替换成其他的数据结构, 比如`B+Tree`等. SkipList的内容比较多，详情移步[大白话解析LevelDB：SkipList（跳表）](https://blog.csdn.net/sinat_38293503/article/details/134643628?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22134643628%22%2C%22source%22%3A%22sinat_38293503%22%7D)

### explicit关键字的作用

至于为什么要将构造函数声明为`explicit`, 是为了防止隐式转换, 保证`Memtable`只能通过显式的方式来构造.
我们看个例子, 如果没有将构造函数声明为`explicit`, 有些错误就会因为隐式转换而变成合法的, 编译阶段无法发现错误.

```c++
class InternalKeyComparator {
  // ...
};

class MemTable {
 public:
  // 没有使用 explicit，允许隐式转换
  MemTable(const InternalKeyComparator& comparator) {
    // 构造 MemTable
  }
  // ...
};

void ProcessMemTable(MemTable mtable) {
  // 处理 MemTable
}

int main() {
  InternalKeyComparator comparator;
  // 不小心写错了, 直接把comparator传给了ProcessMemTable,
  // 但由于没有 explicit, 会隐式创建 MemTable 对象
  ProcessMemTable(comparator);  // 隐式转换
}
```

### 为什么MemTable不允许拷贝

还有两个`MemTable`的构造函数, 被`delete`禁用了, 也就是告诉其他人`MemTable`不允许拷贝和赋值.
为什么不允许拷贝`MemTable`对象呢? 是因为`MemTable`的内存空间是通过内部的`Arena arena_`来进行管理的.
`Arena`是一个内存池, 如果`MemTable`对象被拷贝了, 那么两个`MemTable`对象就会共享同一个`Arena`对象, 造成混乱。
`MemTable`若要支持拷贝, 需要将`arena_`深拷贝, 大大增加了实现的复杂度.

## MemTable::Ref 和 MemTable::Unref

`MemTable`的构造讲完了, 现在来看下`Ref`和`Unref`这两个接口.
`MemTable`的内部有一个`refs_`变量, 用于记录当前`MemTable`的引用计数.
`Ref`方法就是将`refs_`加1, `Unref`方法就是将`refs_`减1, 并且当`refs_`为0时, 销毁`MemTable`对象本身.

```cpp
class MemTable {
   public:
    // ...

    void Ref() { ++refs_; }

    void Unref() {
        --refs_;
        assert(refs_ >= 0);
        if (refs_ <= 0) {
            delete this;
        }
    }

    // ...
};
```

## MemTable::Add 和 MemTable::Get

看下`Add`和`Get`这两个接口分别用于往`MemTable`中添加和查找`key-value`.

### MemTable::Add

`Add`接口接受4个参数, 分别是`sequence`, `type`, `key`和`value`.
将这4个参数编码为一个`entry`, 然后调用`SkipList::Insert`插入到`skiplist`中.
`SkipList::insert`的实现见[SkipList::Insert](https://blog.csdn.net/sinat_38293503/article/details/134643628?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22134643628%22%2C%22source%22%3A%22sinat_38293503%22%7D#SkipListInsert_207)


```cpp
void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
    // MemTable::Add会将{key, value, sequence, type}编码为一个Entry, 然后插入到SkipList中 
    // MemTable Entry的格式如下:
    // |----------------------|----------------------------------|
    // | Field                | Description                      |
    // |----------------------|----------------------------------|
    // | key_size             | varint32 of internal_key.size()  |   <--- head
    // | internal_key bytes   | char[internal_key.size()]        |
    // | value_size           | varint32 of value.size()         |
    // | value bytes          | char[value.size()]               |
    // |----------------------|----------------------------------|
    // 
    // 其中, internal_key的格式如下:
    // |----------------------|----------------------------------|
    // | Field                | Description                      |
    // |----------------------|----------------------------------|
    // | user_key bytes       | char[user_key.size()]            |   <--- head
    // | sequence             | 7 Byte                           |
    // | type                 | 1 Byte                           |
    // |----------------------|----------------------------------|

    // 计算 key 和 value 的大小
    size_t key_size = key.size();
    size_t val_size = value.size();

    // InternalKey = Key + SequenceNumber(7B) + Type(1B)
    // 所以 InternalKey 的大小为 key_size + 8
    size_t internal_key_size = key_size + 8;
    
    // encoded_len是整个entry的大小
    const size_t encoded_len = VarintLength(internal_key_size) +
                               internal_key_size + VarintLength(val_size) +
                               val_size;
    
    // 从arena_中分配内存, 开辟entry的空间, 即buf
    char* buf = arena_.Allocate(encoded_len);

    // 在entry中写入internal_key_size
    char* p = EncodeVarint32(buf, internal_key_size);

    // 在entry中写入key
    std::memcpy(p, key.data(), key_size);
    p += key_size;

    // 在entry中写入sequence与type
    EncodeFixed64(p, (s << 8) | type);
    p += 8;

    // 在entry中写入value_size
    p = EncodeVarint32(p, val_size);

    // 在entry中写入value
    std::memcpy(p, value.data(), val_size);

    // 检查是否刚好将entry填满
    assert(p + val_size == buf + encoded_len);

    // 将entry插入skiplist
    table_.Insert(buf);
}
```

### MemTable::Get

`Get`接口接受一个`LookupKey`对象, 用于查找`key`对应的`value`.

```c++
// LookupKey格式如下:
// |----------------------|-------------------------------------------------|
// | Field                | Description                                     |
// |----------------------|-------------------------------------------------|
// | internal_key_size    | varint32 of internal_key.size()                 |   <--- head
// | user_key bytes       | char[user_key.size()]                           |
// | sequence and type    | 8 bytes (SequenceNumber(7B) and ValueType(1B))  |
// |----------------------|-------------------------------------------------|
bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
    // memkey = internal_key_size + user_key + sequence&&type
    Slice memkey = key.memtable_key();

    // typedef SkipList<const char*, KeyComparator> Table;
    // iter是一个SkipList::Iterator.
    // 创建一个skiplist的iterator.
    Table::Iterator iter(&table_);

    // 把iter移动到memkey所在的位置.
    iter.Seek(memkey.data());

    // 如果找不到对应的memkey, 则返回false.
    // 这里其实可以写的简洁些:
    // if (!iter.Valid()) { return false; }
    if (iter.Valid()) {

        // entry的format在MemTable::Add()中有详细的描述.
        // 取出entry
        const char* entry = iter.key();

        // 取出entry的interal_key_size(key_length), 与user_key的指针(key_ptr)
        uint32_t key_length;
        const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);

        // 检查一下iter seek到的key和lookupkey是不是同一个user_key.
        if (comparator_.comparator.user_comparator()->Compare(
                Slice(key_ptr, key_length - 8), key.user_key()) == 0) {
            
            // 把type(tag)取出来
            const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
            switch (static_cast<ValueType>(tag & 0xff)) {

                // iter seek到的key是一个插入或者更新的key,
                // 把value取出来, return true
                case kTypeValue: {
                    Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
                    value->assign(v.data(), v.size());
                    return true;
                }

                // iter seek到的key已经被标记为删除了
                // 将status设为NotFound, return true
                case kTypeDeletion:
                    *s = Status::NotFound(Slice());
                    return true;
            }
        }
    }
    return false;
}
```

## 为什么没有MemTable::Delete

小朋友, 你是否有个问号, 为什么`MemTable`只实现了`Add`和`Get`, 没有`Delete`呢?

这是因为 LevelDB 使用的是基于日志结构合并树（Log-Structured Merge-tree，简称 LSM-tree）的存储机制，其对删除操作的处理与传统的键值存储系统有所不同。

1. **删除操作的实现**:
   - 在 LSM-tree 中，删除操作被视为一种特殊的插入操作。当你想删除一个键时，LevelDB 实际上会在 `MemTable` 中插入一个带有该键的特殊标记（称为墓碑标记，tombstone）。这个标记表示该键已被删除。也就是通过`MemTable::Add`插入一条`kTypeDeletion`类型的记录。

2. **合并过程中的删除处理**:
   - 当 `MemTable` 被转换到 SSTable（排序字符串表）并进行合并（Compaction）时，带有墓碑标记的键会被用来从较低层的 SSTable 中删除相应的键值对。这意味着删除操作实际上是在合并过程中延迟处理的。

3. **效率和空间考虑**:
   - 通过这种方式处理删除操作，LevelDB 能够在不立即清理数据的情况下快速响应删除请求，同时在后台合并过程中有效地处理实际的数据删除，这样既提高了效率，又节省了存储空间。

`MemTable`除了`Add`和`Get`, 还有一个`NewIterator`与`ApproximateMemoryUsage`两个与数据读写相关的接口.

## MemTable::NewIterator

`NewIterator`用于生成一个`MemTable`的迭代器, 用于遍历`MemTable`中的所有`key-value`.
实现非常简单, 构造一个`MemTableIterator`对象即可.
而`MemTableIterator`其实就是把`SkipList`包装了一层. 具体实现见[SkipList::Iterator的实现](https://blog.csdn.net/sinat_38293503/article/details/134643628?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22134643628%22%2C%22source%22%3A%22sinat_38293503%22%7D#SkipListIterator_601)

```cpp
Iterator* MemTable::NewIterator() { return new MemTableIterator(&table_); }

class MemTableIterator : public Iterator {
   public:
    explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

    MemTableIterator(const MemTableIterator&) = delete;
    MemTableIterator& operator=(const MemTableIterator&) = delete;

    ~MemTableIterator() override = default;

    bool Valid() const override { return iter_.Valid(); }
    void Seek(const Slice& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
    void SeekToFirst() override { iter_.SeekToFirst(); }
    void SeekToLast() override { iter_.SeekToLast(); }
    void Next() override { iter_.Next(); }
    void Prev() override { iter_.Prev(); }
    Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
    Slice value() const override {
        Slice key_slice = GetLengthPrefixedSlice(iter_.key());
        return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
    }

    Status status() const override { return Status::OK(); }

   private:
    // MemTable::Table::Iterator = SkipList::Iterator
    MemTable::Table::Iterator iter_;
    std::string tmp_;  // For passing to EncodeKey
};
```

## MemTable::ApproximateMemoryUsage

`ApproximateMemoryUsage`用于获取`MemTable`的内存占用, 是个预估值.
实际返回的是`arena_`的内存占用, 因为`MemTable`的所有内存都是通过`arena_`来分配的,
具体实现见[Arena](https://blog.csdn.net/sinat_38293503/article/details/134697829)

```cpp
size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }
```

至此, `MemTable`的实现就过完了.