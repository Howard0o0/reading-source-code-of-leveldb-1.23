# ShardedLRUCache

`ShardedLRUCache`是`Cache`的一种实现，所以在看`ShardedLRUCache`的实现之前，我们需要先了解下`Cache`的定义。

## Cache 接口定义

我们可以先看下`Cache`的使用姿势，再来看`Cache`的接口定义。

```cpp
Cache* cache = NewLRUCache(100);
Handle* handle = cache->Insert("key", "value", 5, nullptr);
cache->Erase("key"); // 即使这里将缓存项从 Cache 中移除了，但该缓存项还是会保留在内存中
cache->Value(handle); // 此时还是可以通过 handle 获取到缓存项的 Value
cache->Release(handle); // 这里将缓存项的引用计数减一，如果引用计数为 0，那么该缓存项会被销毁。
```

`Cache`的接口定义如下：

```cpp
class LEVELDB_EXPORT Cache {
   public:
    Cache() = default;

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    // 用户调用 Insert() 方法插入一个缓存项时，会同时传入一个 deleter。
    // 当 Cache 析构时，会调用所有缓存项的 deleter 来对 Cache 里的
    // 所有缓存项进行清理。
    virtual ~Cache();

    // 声明了一个抽象的 Handle 类型，用于表示 Cache 中的一个缓存项。
    struct Handle {};

    // 这个方法用于向 Cache 中插入一对 Key-Value，Cache 的实现类会在内部
    // 基于这对 Key-Value 生成一个 Handle 对象，将该 Handle 插入到 Cache
    // 中，并把这个 Handle 的指针返回给用户。
    //
    // 返回 Handle 指针，相当于这个 Cache 缓存项的引用计数加一，即使另外一个线程
    // 将这个缓存项从 Cache 中移除了，该缓存项还是会保留在内存中。
    // 用户需要在使用完这个缓存项后，调用 Release() 方法来将该缓存项的引用计数减一。
    // 可以通过以下例子来理解:
    //      Cache* cache = NewLRUCache(100);
    //      Handle* handle = cache->Insert("key", "value", 5, nullptr);
    //      cache->Erase("key"); // 即使这里将缓存项从 Cache 中移除了，但该缓存项还是会保留在内存中
    //      cache->Value(handle); // 此时还是可以通过 handle 获取到缓存项的 Value
    //      cache->Release(handle); // 这里将缓存项的引用计数减一，如果引用计数为 0，那么该缓存项会被销毁。
    //
    // charge 参数表示这个缓存项的大小，因为缓存项里只存储了 value 的指针，所以计算出
    // 该缓存项的大小，需要用户告知 Cache。
    virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                           void (*deleter)(const Slice& key, void* value)) = 0;

    // 通过 key 查找 Cache 中的缓存项:
    //      - 如果找到了，返回该缓存项的 Handle 指针。
    //      - 同时将该缓存项的引用计数加一。如果没有找到，返回 nullptr。
    virtual Handle* Lookup(const Slice& key) = 0;

    // 将缓存项的引用计数减一，如果引用计数为 0，那么该缓存项会被销毁。
    virtual void Release(Handle* handle) = 0;

    // 返回缓存项的 Value。
    virtual void* Value(Handle* handle) = 0;

    // 指定一个 key，从 Cache 中移除一个缓存项。
    // 如果该缓存项的引用计数为 0，那么该缓存项会被销毁。
    virtual void Erase(const Slice& key) = 0;

    // 返回一个新的数字，作为该 Cache 的 ID。
    virtual uint64_t NewId() = 0;

    // 移除 Cache 中所有没有被使用的缓存项，也就是引用计数为 0 的那些。
    virtual void Prune() {}

    // 计算 Cache 中所有缓存项的大小之和。
    virtual size_t TotalCharge() const = 0;
};
```


## ShardedLRUCache 的实现

### ShardedLRUCache 的构造函数

`ShardedLRUCache`内部有一个`LRUCache`数组，`LRUCache shard_[kNumShards]`。

意思是`ShardedLRUCache`不是一个大`Cache`，而是将这个大的`Cache` shard 为多个小`Cache`，每个小`Cache`叫做一个`shard`。

所以叫做`ShardedLRUCache`。

```cpp
// capacity: ShardedLRUCache 的总容量
// 根据总容量计算每个 shard 里的 LRUCache 的容量。
explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    // 计算 per_shard: 每个 shard 的容量。
    // per_shard = ⌈capacity / kNumShards⌉ (向上取整)
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
        // 给每个 shard 里的 LRUCache 设置容量。
        shard_[s].SetCapacity(per_shard);
    }
}
```

### 