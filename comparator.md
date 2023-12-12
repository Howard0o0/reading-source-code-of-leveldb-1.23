# LevelDB 中的 Comparator

LevelDB 中 将 Key 之间的比较抽象为 Comparator，方便用户根据不同的业务场景实现不同的比较策略。

## Comparator 接口概述

```cpp
class LEVELDB_EXPORT Comparator {
   public:
    virtual ~Comparator();

    // 用于比较 Key a 和 Key b。
    //   - 如果 a < b，返回值 < 0；
    //   - 如果 a == b，返回值 == 0；
    //   - 如果 a > b，返回值 > 0。
    virtual int Compare(const Slice& a, const Slice& b) const = 0;

    // Comparator 的名称，用于检查 Comparator 是否匹配。
    // 比如数据库创建时使用的是 Comparator A，
    // 重新打开数据库时使用的是 Comparator B，
    // 此时 LevelDB 则会检测到 Comparator 不匹配。
    virtual const char* Name() const = 0;

    // 找到一个最短的字符串 seperator，使得 start <= seperator < limit，
    // 并将结果保存在 start 中。
    // 用于 SST 中 Data Block 的索引构建。
    virtual void FindShortestSeparator(std::string* start, const Slice& limit) const = 0;

    // 找到一个最短的字符串 successor，使得 key <= successor，
    // 并将结果保存在 key 中。
    // 用于 SST 中最后一个 Data Block 的索引构建，
    // 因为没有后续的 Key 了。
    virtual void FindShortSuccessor(std::string* key) const = 0;
};
```

## LevelDB 中 Comparator 的默认实现：BytewiseComparatorImpl

在 LevelDB 中，如果用户没有指定 Comparator，那么 LevelDB 将使用默认的 Comparator：BytewiseComparatorImpl。

## BytewiseComparatorImpl::Compare(const Slice& a, const Slice& b)

```c++
int Compare(const Slice& a, const Slice& b) const override { return a.compare(b); }
```

`BytewiseComparatorImpl::Compare(const Slice& a, const Slice& b)`的实现非常简单，直接甩给了 `Slice::compare(const Slice& b)`。

```c++
inline int Slice::compare(const Slice& b) const {
    // 计算两个`Slice`的最小长度
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    // 使用`memcmp`比较两个字符串的前 min_len 个字符
    int r = memcmp(data_, b.data_, min_len);
    // 如果前 min_len 个字符相等，则比较两个字符串的长度
    if (r == 0) {
        if (size_ < b.size_)
            r = -1;
        else if (size_ > b.size_)
            r = +1;
    }
    return r;
}
```

`Slice::compare(const Slice& b)`就是使用了`memcmp`来比较。`memcmp`是一个 C 标准库函数，按字节序比较两个字符串的前 n 个字符。

## BytewiseComparatorImpl::FindShortestSeparator(std::string* start, const Slice& limit)

`BytewiseComparatorImpl::FindShortestSeparator`的思路就是找到`start`与`limit`第一个不相同字符的下标`diff_index`， 然后将`start[diff_index]`加1，`start[:diff_index]`就是最短的分隔符。

```c++
void FindShortestSeparator(std::string* start, const Slice& limit) const override {
    // 先计算出 start 和 limit 的最短长度
    size_t min_length = std::min(start->size(), limit.size());
    // diff_index 用于记录 start 和 limit 第一个不同的字符的位置
    size_t diff_index = 0;
    while ((diff_index < min_length) && ((*start)[diff_index] == limit[diff_index])) {
        diff_index++;
    }

    if (diff_index >= min_length) {
        // 走到这就表示 start 是 limit 的前缀。
        // 这种 case 下，start 本身就是最短的分隔符，可以直接返回。
    } else {
        // 获取 start 的 第 diff_index 个字符
        uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
        // 先判断 diff_byte < 0xff 的意义在于
        // 防止 diff_byte + 1 溢出，导致结果不正确。
        if (diff_byte < static_cast<uint8_t>(0xff) &&
            diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
            // start 与 limit 的前 diff_index 个字符都相同，
            // 只需要把 start 的第 diff_index 个字符加 1 ，
            // 就得到 Shortest Seperator 了。
            (*start)[diff_index]++;
            start->resize(diff_index + 1);
            assert(Compare(*start, limit) < 0);
        }
    }
}
```

## BytewiseComparatorImpl::FindShortSuccessor(std::string* key)

`FindShortSuccessor`用于构建 SST 中最后一个 Data Block 的索引，因为没有后续的 Key 了。

```c++
void FindShortSuccessor(std::string* key) const override {
    // Find first character that can be incremented
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
        const uint8_t byte = (*key)[i];
        // 找到第一个小于 0xff 的字符，然后加 1
        // 以该字符为上界。 
        if (byte != static_cast<uint8_t>(0xff)) {
            (*key)[i] = byte + 1;
            key->resize(i + 1);
            return;
        }
    }
    // *key is a run of 0xffs.  Leave it alone.
}
```