# Arena

大多数C++相关的开源项目都会实现自己的内存管理，而不直接使用标准库的`malloc`。 现在流行的`malloc`实现有`jemalloc`、`tcmalloc`、`ptmalloc`等，它们都能高效的管理内存，减少内存碎片，提高多线程下内存分配的性能。但这都是对应于general的应用场景，每个应用都有不同的场景，有的分配大内存为主，有的分配小内存为主。为了更进一步的提高内存分配效率与减少内存碎片，LevelDB使用自己的内存管理机制，Arena。

相比于直接使用`malloc`，Arena的优势在于：

1. **小内存分配的效率**：LevelDB经常需要分配小块内存。对于每个小分配使用`malloc`可能因为开销而效率不高。“Arena”一次性分配较大的内存块，然后高效地分割出小段。这减少了许多小`malloc`调用的开销。 这种批处理机制同样应用于`jemalloc`, `tcmalloc`等，但他们需要维护复杂的数据结构，不如“Arena”简单高效。

2. **方便统计内存使用情况**：LevelDB需要跟踪内存使用情况，通过Arena而不是直接`malloc`可以方便的记录内存使用情况。

3. **控制内存生命周期**：“Arena”允许LevelDB轻松控制其内存分配的生命周期。当Arena被销毁时，它分配的所有内存都在一次操作中被释放，这比单独释放每个块更有效。

我们来看下`class Arena`的结构：

```cpp
class Arena {
   public:
    Arena();

    // 不允许拷贝
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    ~Arena();

    // 等同于malloc(bytes)
    char* Allocate(size_t bytes);

    // 内存对齐版的Allocate
    char* AllocateAligned(size_t bytes);

    // 返回至今为止分配的内存总量
    size_t MemoryUsage() const;
};
```

`Arena`提供了两个版本的内存分配函数，一个是`Allocate`，一个是`AllocateAligned`。前者不考虑内存对齐，后者会自动进行内存对齐。内存对齐的好处后面再讲。

`Arena`只提供了内存申请的`Allocate`接口，却没有内存释放的接口，那怎么才能释放内存呢？当`Arena`销毁的时候，再集中释放。🤣

现在我们逐个分析各个接口的实现。

## Allocate(size_t bytes)的实现

先来认识下`Arena`里的3个成员:
```c++
class Arena {
    // ...

    // Arena每次都从OS申请内存都是申请
    // 一个block，然后放到blocks_里。
    std::vector<char*> blocks_;

    // 当前block里，下一次分配内存返回的地址
    char* alloc_ptr_;
    // 当前block里，剩余可分配的bytes
    size_t alloc_bytes_remaining_;
};
```

现在再来看`Allocate`的实现：

```cpp
inline char* Arena::Allocate(size_t bytes) {
    // Allocate(0)没有意义
    assert(bytes > 0);

    // 如果申请的bytes小于当前block剩余的bytes，
    // 就从block的剩余bytes里分配。
    // 并更新alloc_ptr_和alloc_bytes_remaining_
    if (bytes <= alloc_bytes_remaining_) {
        // 当前block的alloc_ptr_就是需要返回的地址，
        char* result = alloc_ptr_;
        // 更新下一次分配的起始地址
        alloc_ptr_ += bytes;
        // 更新当前block剩余可分配的bytes
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    // 如果申请的bytes大于当前block剩余的bytes，
    // 使用AllocateFallback(bytes)分配
    return AllocateFallback(bytes);
}
```

`AllocateFallback(bytes)`用于当前block剩余内存不足的情况。

这里有个前置知识需要补充一下，在分配内存时，为了避免内存碎片，一般会设两个内存池：大内存池和小内存池。分配大内存时走大内存池，分配小内存时走小内存池。

`AllocateFallback`的思路是大内存直接找os要，小内存就从自己的block里分配。

这样做的好处是：

- 申请大内存的几率比较小，不会很频繁，找os要虽然慢但是可以避免内存碎片。
- 申请小内存的几率大，会比较频繁，从block中分配，效率高并且碎片也少。 

```cpp
char* Arena::AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) {
        // 当分配的内存大于块的1/4时，直接找os要，不从block中分配，
        // 以此保证将每个block的内存碎片限制在1/4以内。
        // 找os要一个 bytes 大小的block，这个
        // block不再用于后续分配内存，用户单独享用。
        // 
        // 其实这里是可以直接写成 return malloc(bytes)的，
        // 只是为了记录内存分配情况，所以要走一遍AllocateNewBlock。
        char* result = AllocateNewBlock(bytes);
        return result;
    }

    // 只有bytes小于当前block剩余的bytes时才会走到AllocateFallback，
    // 所以此时肯定要找os要一个新的block。
    alloc_ptr_ = AllocateNewBlock(kBlockSize);
    alloc_bytes_remaining_ = kBlockSize;

    // 更新 alloc_ptr_ 与 alloc_bytes_remaining_ 
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
}
```

继续看`AllocateNewBlock`的实现，代码写的很清晰了，不需要解释。

```cpp
char* Arena::AllocateNewBlock(size_t block_bytes) {
    // 找os拿一块block_bytes大小的内存，
    char* result = new char[block_bytes];

    // 将该block放到blocks_中，
    // Arena销毁的时候一起释放。
    blocks_.push_back(result);

    // 记录分配的内存量。
    memory_usage_.fetch_add(block_bytes + sizeof(char*),
                            std::memory_order_relaxed);
    return result;
}
```

最后值得一提的是，为什么`AllocateFallback`中的`AllocateNewBlock(kBlockSize)`，找os申请新block时，默认要申请`kBlockSize = 4K`的大小呢？ 

将`kBlockSize`设为`4K`，有以下几点好处：

- 减少内存碎片：linux的内存管理里，每次内存申请都以页为单位，页的大小是`4KB`。比如说从os拿`5KB`的内存，操作系统实际上会分配2页的内存，也就是`8KB`，这就会产生`3KB`的内存碎片。而如果每次申请的内存都是`4KB`的整数倍，os就会刚好分配1页，不会产生内存碎片。

- 减少`Page Fault`的开销：`4KB`意味着这段内存空间位于一张页面上，只需做1次`Page Fault`。若将`4KB`改为`4100B`，访问最后`10B`的时候，由于这`10B`不在一张页面上，需要产生2次`Page Fault`。

- 更好利用CPU缓存： 1个`cache-line`的大小是64B，`4KB`刚好是`64B`的整数倍，连续的数据块更有可能完全位于单个`cache-line`内。

- 降低`Cache False-Sharing`的概率：关于`Cache False-Sharing`详见[这里](https://blog.csdn.net/weixin_30270561/article/details/99394006)。

## AllocateAligned(size_t bytes)的实现

`AllocateAligned`与`Allocate`相比，保证分配的内存是对齐的。

比如当前`alloc_ptr_`指向的是`0x1010`，也就是`10`，而平台的字长是`8Byte`，那如果从`0x1010`开始分配8个字节，范围是`10 ~ 17`，这样就不是对齐的了。CPU需要取两次内存，一次取`10 ~ 15`，一次取`15 ~ 17`，这样就会降低效率。

`AllocateAligned`会先将`alloc_ptr_`移动到`16`， 然后再分配8个字节，这样就是对齐的了。

```c++
char* Arena::AllocateAligned(size_t bytes) {
    // 计算对齐的长度，最小对齐长度为8。
    // 如果当前平台的字长大于8，则对齐长度为字长。
    const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
    
    // x & (x-1) 是个位运算的技巧，用于快速的将x的最低一位1置为0。
    // 比如x = 0b1011, 则x & (x-1) = 0b1010。
    // 此处用(align & (align - 1)) == 0)快速判断align是否为2的幂。
    // 因为2的幂的二进制表示总是只有一位为1，所以x & (x-1) == 0。
    static_assert((align & (align - 1)) == 0,
                  "Pointer size should be a power of 2");

    // 位运算技巧，等同于 current_mod = alloc_ptr_ % align
    size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);

    // 为了对齐，多分配slop个字节。
    size_t slop = (current_mod == 0 ? 0 : align - current_mod);
    size_t needed = bytes + slop;

    char* result;
    if (needed <= alloc_bytes_remaining_) {
        // 向后移动alloc_ptr_, 
        // 将alloc_ptr_对齐到align的整数倍。
        result = alloc_ptr_ + slop;
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
    } else {
        // AllocateFallback本身就是对齐的，所以直接调用即可。
        // 因为AllocateFallback要么从os直接分配,
        // 要么new一个4KB的block，返回block的首地址
        result = AllocateFallback(bytes);
    }
    assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
    return result;
}
```

## MemoryUsage()的实现

`MemoryUsage()`很简单，直接返回`memory_usage_`即可。 

```c++
size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
}
```

在`AllocateNewBlock`中，每次分配内存都会更新`memory_usage_`。

```c++
char* Arena::AllocateNewBlock(size_t block_bytes) {
    // ...

    // 记录分配的内存量。
    memory_usage_.fetch_add(block_bytes + sizeof(char*),
                            std::memory_order_relaxed);
    // ...
}
```

为什么更新`memory_usage_`和读取`memory_usage_`使用的都是`std::memory_order_relaxed`呢？

因为`memory_usage_`的上下文里没有需要读取`memory_usage_`的地方，不需要对指令重排做约束。

至此，`Arena`的实现就分析完了。

## Arena的内存释放

当`Arena`对象销毁时，会集中销毁`blocks_`里的block，释放内存。

```c++
Arena::~Arena() {
    for (size_t i = 0; i < blocks_.size(); i++) {
        delete[] blocks_[i];
    }
}
```

## AllocateAligned && Allocate 的适用场景

`LevelDB`中，用到`Arena`的只有两个地方:

- `MemTable::Add`里使用`Arena::Allocate`分配代插入记录的内存
- `SkipList::NewNode`里使用`Arena::AllocateAligned`分配`SkipList::Node`的内存

```c++
void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {

    // ...

    // 通过Allocate分配record(key+value)的内存
    char* buf = arena_.Allocate(encoded_len);

    // ...
}
```

```c++
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
    // 通过AllocateAlgined分配node的内存
    char* const node_memory = arena_->AllocateAligned(
        sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
    // 这里是 placement new 的写法，在现有的内存上进行 new object
    return new (node_memory) Node(key);
}
```

为什么前者使用`Allocate`，后者使用`AllocateAligned`呢？

`MemTable::Add`用于往`MemTable`中插入记录，这条记录的内存即使没对齐也没关系，因为不会对这块不会像遍历数组那样挨个访问，只是开辟一块内存把东西写进去，然后基本就不会访问这块内存了。若强行使用`AllocateAligned`只会徒增内存碎片。

而`SkipList::Node`就不一样了，`SkipList::Node`里有个`next_[]`数组，`next_[]`会被频繁读取。如果`next_[]`里某个元素不是对齐的，每次取这个元素的时候CPU都需要取两次内存，并且会增加`Cache False-Sharing`的概率，关于`Cache False-Sharing`详见[这里](https://blog.csdn.net/weixin_30270561/article/details/99394006)。所以`SkipList::Node`的内存需要使用`AllocateAligned`分配。



