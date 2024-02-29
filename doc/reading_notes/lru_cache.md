# LRUCache

`LRUCache`是一个基于LRU（Least Recently Used）算法实现的`Cache`。

当`Cache`满了之后，再插入新的缓存项时，会将`Cache`中访问时间最早的缓存项移除，为新的缓存项腾出空间。

## LRUCache 的实现思路

`LRUCache`由 3 个数据结构组成，2 个链表和 1 个哈希表:

```cpp
class LRUCache {
   
private:
    // ...

    // LRU 链表的 Dummy Head 节点。
    // LRU 链表中存放 refs == 1 && in_cache == true 的缓存项。
    // LRU 链表中，最新的缓存项是尾节点，最老的是头节点。
    LRUHandle lru_ GUARDED_BY(mutex_);

    // in_use 链表的 Dummy Head 节点。
    // in_use 链表中的缓存项是正在被客户端使用的，它们的引用次数 >= 2，in_cache==true。
    LRUHandle in_use_ GUARDED_BY(mutex_);

    // Cache 中所有缓存项的 Hash 表，用于快速查找缓存项。
    HandleTable table_ GUARDED_BY(mutex_);
};
```

我们先来看`lru_`链表和`table_`哈希表，这两个数据结构是`LRUCache`的核心，`in_use_`链表稍后再说。

### lru_ 链表

`lru_`链表是一个双向链表，用于存放`Cache`中的缓存项。

链表节点用`LRUHandle`来表示(感觉如果叫做`LRUNode`的话会更好理解)，`lru`链表的示意图如下:

```plaintext
   Older
<-----------------------------------------------------------------------------------------------+


+------------+             +------------+              +------------+              +------------+              +------------+
|            | +---------> |            |  +---------> |            |  +---------> |            |  +---------> |            |
|  LRUHandle |             |  LRUHandle |              |  LRUHandle |              |  LRUHandle |              |  LRUHandle |
|            | <---------+ |            |  <---------+ |            |  <---------+ |            |  <---------+ |            |
+------------+             +------------+              +------------+              +------------+              +------------+
                                                                                                                    lru_
+------------------------------------------------------------------------------------------------>
                                                                                      Younger
```

`lru_`链表的头节点是一个 Dummy Head 节点，不存放任何数据，只是用来简化链表的操作。

`lru_`链表的头部的节点，是访问时间最新的节点，而越靠近尾部的节点，访问时间越早。

当需要往`Cache`中插入新节点的时候，会使用头插法将该节点插入到`lru_`链表的头部。

如果`Cache`满了，需要移除一些老节点为新节点腾出空间，就会从`lru_`链表的尾部开始移除节点，直到空间足够插入新节点位置。

这样一来，往`Cache`中插入新节点就解决了，只需要$O(1)$的时间往`lru_`链表的头部插入即可。

那么怎么从`Cache`中快速查找一个缓存项呢？这就需要用到`table_`哈希表了。

### table_ 哈希表

`table_`哈希表是一个`HandleTable`，用于快速查找`Cache`中的缓存项。

LevelDB 设计的`LRUHandle`很巧妙，`LRUHandle`中存储了该缓存项的`key`、`value`、`hash`等信息。

其中`key`是这个缓存项的唯一标识，`hash`是`key`的哈希值，`value`是缓存项的值。

使用`table_.Lookup(key, hash)`可以在$O(1)$的时间复杂度内查找到`key`对应的缓存项在`lru_`链表中的位置。

也就是说，往`Cache`中插入一个`Key-Value`时，会构建出一个`LRUHandle`插入到`lru_`链表的头部，同时会在`table_`哈希表中插入`{key, LRUHandle}`。

这样在查找`key`对应的缓存项时，只需要在`table_`哈希表中查找即可，不需要遍历整个`lru_`链表。

如果要从`Cache`中删除某个`key`对应的缓存项，也只需要在`table_`哈希表中查找到`key`对应的`LRUHandle`所在位置，然后从`lru_`链表中移除即可。

如此一来，通过`table_`哈希表和`lru_`链表的相互配合，就已经可以实现一个高效的`LRUCache`了，其增删改查的时间复杂度都是$O(1)$。

那么`in_use_`链表是干什么的呢？

### in_use_ 链表

与`lru_`链表一样，`in_use_`链表也是一个双向链表，用于存放`Cache`中的缓存项。

什么样的缓存项会被放到`in_use_`链表中呢？

我们来看这样一个场景:

`LRUCache* cache`里有`{LRUHandle_1, LRUHandle_2, LRUHandle_3}`三个缓存项，他们的`Key`分别为`"key1"`, `"key2"`, `"key3"`，且引用计数都为`1`。

此时客户端从`cache`里获取了`key1`和`key2`的缓存项:

```cpp
LRUHandle* lruhandle_1 = cache->Lookup("key1");
LRUHandle* lruhandle_2 = cache->Lookup("key2");

// lruhandle_1 和 lruhandle_2 正在被客户端使用..
```

会让`lruhandle_1`和`lruhandle_2`的引用计数加一，此时`lruhandle_1`和`lruhandle_2`的引用计数都变为了`2`，那么他们就会从`lru_`链表中移出来，放到`in_use_`链表中。

当`lruhandle_1`和`lruhandle_2`被客户端使用完毕后，通过`LRUCache::Release(lruhandle)`方法将他们的引用计数减一。

```cpp
cache->Release(lruhandle_1);
cache->Release(lruhandle_2);
```

此时`lruhandle_1`和`lruhandle_2`的引用计数都变回为了`1`，会从`in_use_`链表中移出来，又放回到`lru_`链表中。

`in_use_`链表的作用是让我们能清晰的知道哪些缓存项是正在被客户端使用的，哪些是在`Cache`中但是没有正在被使用，这样可以实现更精细的缓存策略。

比如在`LRUCache::Prune()`方法中，可以将所有没有正在被使用的缓存项从`Cache`中移除。

## LRUCache 的代码实现

### LRUCache 的定义

我们先来看下`LRUCache`的定义，都有哪些公共接口:

```cpp
class LRUCache {
   public:
    LRUCache();
    ~LRUCache();

    // 设置 Cache 的容量。
    // 当插入一条缓存项使得 Cache 的总大小超过容量时，会将最老(访问时间最早)的缓存项移除。
    void SetCapacity(size_t capacity) { capacity_ = capacity; }

    // 插入一个缓存项到 Cache 中，同时注册该缓存项的销毁回调函数。
    // key: 缓存项的 key
    // hash: key 的 hash 值，需要客户端自己计算
    // value: 缓存数据的指针
    // charge: 缓存项的大小，需要客户端自己计算，因为缓存项里只存储了缓存数据的指针
    // deleter: 缓存项的销毁回调函数
    Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value, size_t charge,
                          void (*deleter)(const Slice& key, void* value));
    
    // 根据 key 和 hash 查找缓存项。
    Cache::Handle* Lookup(const Slice& key, uint32_t hash);

    // 将缓存项的引用次数减一。
    void Release(Cache::Handle* handle);

    // 将缓存项从 Cache 中移除。
    void Erase(const Slice& key, uint32_t hash);

    // 移除 Cache 中所有没有正在被使用的缓存项，也就是引用计数为 1 的那些。
    void Prune();

    // 返回 Cache 里所有缓存项的总大小，也就是 Cache 的占用的内存空间。
    size_t TotalCharge() const {
        MutexLock l(&mutex_);
        return usage_;
    } 

   private:
    // LRUCache 的 3 个核心数据结构:

    // LRU 链表的 Dummy Head 节点。
    // LRU 链表中存放 refs == 1 && in_cache == true 的缓存项。
    // LRU 链表中，最新的缓存项是尾节点，最老的是头节点。
    LRUHandle lru_ GUARDED_BY(mutex_);

    // in_use 链表的 Dummy Head 节点。
    // in_use 链表中的缓存项是正在被客户端使用的，它们的引用次数 >= 2，in_cache==true。
    LRUHandle in_use_ GUARDED_BY(mutex_);

    // Cache 中所有缓存项的 Hash 表，用于快速查找缓存项。
    HandleTable table_ GUARDED_BY(mutex_);
};
```

### LRUHandle

`LRUHandle`是`LRUCache`的核心数据结构，用于表示`Cache`中的缓存项，把它叫做`LRUNode`可能更好理解一些，`lru_`链表和`in_use_`链表都是由若干个`LRUHandle`节点组成的。

忘记`lru_`链表长什么样的同学可以回头看下[`lru`链表的示意图](https://blog.csdn.net/sinat_38293503/article/details/136381384?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22136381384%22%2C%22source%22%3A%22sinat_38293503%22%7D#lru___32)

在往下看`LRUCache`各个接口的实现之前，我们先来看下`LRUHandle`的定义:

```cpp
struct LRUHandle {
    void* value;
    void (*deleter)(const Slice&, void* value);
    LRUHandle* next_hash; // 如果两个缓存项的 hash 值相同，那么它们会被放到一个 hash 桶中，next_hash 就是桶里的下一个缓存项
    LRUHandle* next; // LRU 链表中的下一个(更新的)缓存项
    LRUHandle* prev; // LRU 链表中的上一个(更旧的)缓存项
    size_t charge;  // 该缓存项的大小
    size_t key_length; // key 的长度
    bool in_cache;     // 该缓存项是否还在 Cache 中
    uint32_t refs;     // 引用次数 
    uint32_t hash;     // key 的 hash 值
    char key_data[1];  // key

    Slice key() const {
        // next_ is only equal to this if the LRU handle is the list head of an
        // empty list. List heads never have meaningful keys.
        assert(next != this);

        return Slice(key_data, key_length);
    }
};
```

#### LRUHandle::key, LRUHandle::hash, LRUHandle::value

`LRUHandle::key`是该缓存项的`Key`，`LRUHandle::hash`是`key`的哈希值，而`LRUHandle::value`是是缓存数据的指针。

`LRUHandle::value`是`void*`类型的，可以存储任意类型的数据，客户端需要自己管理它的生命周期。

#### LRUHandle::next_hash

`LRUHandle::next_hash`可能会有同学还没搞懂，它是用来解决哈希冲突的。

前面我们讲到，`LRUCache`由`table_`哈希表和`lru_`链表组成。

当我们往`LRUCache`中插入一个缓存项`LRUHandle`时，会将该`LRUHandle`往`lru_`链表里插入，同时也会往`table_`哈希表里插入`{key, LRUHandle}`。

示意图如下:

```plaintext
+-----------------------------------------------------------------------------------+
|   +----------------+  +----------------+ +----------------+ +----------------+    |
|   |    Bucket1     |  |    Bucket2     | |    Bucket3     | |    Bucket4     |    |
|   |                |  |                | |                | |                |    |
|   | +------------+ |  | +------------+ | | +------------+ | | +------------+ |    |
|   | | LRUHandle1 | |  | | LRUHandle4 | | | | LRUHandle5 | | | | LRUHandle6 | |    |
|   | +------------+ |  | +------------+ | | +------------+ | | +------------+ |    |
|   |       |next_hash  |                | |                | |                |    |
|   |       |        |  |                | |                | |                |    |
|   | +-----v------+ |  |                | |                | |                |    |
|   | | LRUHandle2 | |  |                | |                | |                |    |
|   | +------------+ |  |                | |                | |                |    |
|   |       |next_hash  |                | |                | |                |    |
|   |       |        |  |                | |                | |                |    |
|   | +-----v------+ |  |                | |                | |                |    |
|   | | LRUHandle3 | |  |                | |                | |                |    |
|   | +------------+ |  |                | |                | |                |    |
|   +----------------+  +----------------+ +----------------+ +----------------+    |
|                                                                                   |
|                                  LRUCache::table_                                 |
+-----------------------------------------------------------------------------------+
```

假设`LRUHandle1`，`LRUHandle2`和`LRUHandle3`的`Key`互不相同，分别为`key1`, `key2`, `key3`，但它们的哈希值恰好都是`1`，那么它们会被一起放到`Bucket1`的链表中，然后用`next_hash`依次连起来。

当我们要在`table_`寻找`Key`为`key2`的`LRUHandle`时，会先计算`key2`的哈希值，找到对应的`Bucket`，也就是`Bucket1`。

#### LRUHandle::next, LRUHandle::prev

`LRUHandle::next`和`LRUHandle::prev`是`LRUHandle`的双向链表指针，用于构成`lru_`链表，这应该比较好理解，见下图。

```plaintext
+------------+      next    +------------+
|            |  +---------> |            |
|  LRUHandle |              |  LRUHandle |
|            |  <---------+ |            |
+------------+      prev    +------------+
```

#### LRUHandle::charge

由于`LRUHandle`中只存储了`value`的指针，无法自己计算出`value`的大小，所以需要客户端自己计算出`value`的大小，然后记录到`LRUHandle::charge`中。

#### LRUHandle::in_cache

若`LRUHandle::in_cache`为`true`，则表示该缓存项还在`Cache`中，可能在`lru_`链表中，也可能在`in_use_`链表中。

#### LRUHandle::refs

该缓存项的引用次数，当引用次数为`1`时，表示该缓存项还在`Cache`中，但是没有正在被客户端使用。

当引用次数大于`1`时，表示该缓存项正在被客户端使用。

#### LRUHandle::deleter

当该缓存项被移出`Cache`时，会查看下该缓存项的引用计数是否为`1`，如果是的话，会调用`LRUHandle::deleter`来销毁缓存项中的缓存数据，也就是`value`。

如果移出时引用计数不为`1`，那么暂时先不调用`LRUHandle::deleter`来将`value`销毁，因为还有客户端在使用这个缓存项。

当客户端使用完毕后，会调用`LRUCache::Release(LRUHandle*)`来将引用计数减一，当引用计数减为`0`了，则调用`LRUHandle::deleter`来销毁`value`。

#### LRUHandle::key_data, LRUHandle::key_length

以数组的方式存储`key`，`LRUHandle::key_data`存储`key`的内容，`LRUHandle::key_length`存储`key`的长度。

但为什么`key_data[1]`的长度只有`1`呢？这是 C/C++ 的一个常用技巧，感兴趣的可以移步翻看[柔性数组](https://blog.csdn.net/sinat_38293503/article/details/134643628#Flexible_Array_Member_500)。

### HandleTable

了解完`LRUHandle`之后，在看`LRUCache`的实现之前，我们还需要来看下`HandleTable`的定义。

前面我们说过，`LRUCache`的核心数据结构是`lru_`链表和`table_`哈希表，`table_`哈希表是由`HandleTable`实现的。

`HanldeTable`是一个用数组实现的哈希表，数组里存放的是`LRUHandle*`，`LRUHandle`的指针。

```cpp
class HandleTable {
    // ...
private:
    uint32_t length_;   // 哈希表数组 list_[] 的大小
    uint32_t elems_;    // 哈希表中存放的元素个数
    LRUHandle** list_; // 哈希表的数组 list_[]
};
```

为什么哈希表中装的是`LRUHandle*`而不是`LRUHandle`呢？

节省空间呀，`lru_`链表已经存了`LRUHandle`了，`table_`哈希表只需要存`LRUHandle`的指针就行了。　

我们来看下 `HandleTable` 的核心接口:

```cpp
class HandleTable {
   public:
    HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
    ~HandleTable() { delete[] list_; }

    LRUHandle* Lookup(const Slice& key, uint32_t hash);

    // 插入一个新的 LRUHandle, 返回一个和这个新 LRUHandle 相同 Key 的老 LRUHandle，
    // 如果存在的话。
    LRUHandle* Insert(LRUHandle* h);

    // 从哈希表中移除一个指定 Key 的 LRUHandle。
    LRUHandle* Remove(const Slice& key, uint32_t hash);
};
```

#### HandleTable::Insert(LRUHandle* h)

先查找待插入项`h`在哈希表中的待插入位置，然后对该位置使用反引用赋值。

假设`h`应该插入到`list_[i]`的位置，通过`LRUHandle** ptr = FindPointer(h->key(), h->hash)`获取到`list_[i]`的地址，然后对`*ptr`进行赋值。

`*ptr = h`相当于`list_[i] = h`，这样就完成了`h`的插入。

```cpp
LRUHandle* Insert(LRUHandle* h) {
    // 找到 key 对应的 LRUHandle* 在 Hash 表中的位置。
    // 如果哈希表中存在相同 key 的缓存项，那么返回老的 LRUHandle* 
    // 在 Hash 表中的位置。
    // 如果哈希表中不存在相同 key 的缓存项，那么返回新的 LRUHandle*
    // 需要插入到 Hash 表中的位置。
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    
    // 先把老的 LRUHandle* 保存下来，最后返回给客户端。
    LRUHandle* old = *ptr;
    // 如果 old 存在，就用新的 LRUHandle* 替换掉 old。
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
        /// 如果 old 不存在，表示哈希表中需要新插入一个 LRUHandle*。
        // 此时需要更新哈希表的元素个数，如果元素个数超过了哈希表的长度，
        // 则需要对哈希表进行扩容。
        ++elems_;
        if (elems_ > length_) {
            Resize();
        }
    }
    return old;
}
```

#### HandleTable::Lookup(const Slice& key, uint32_t hash)

使用`FindPointer`来查找`key`对应的`LRUHandle*`在哈希表中的位置。

```cpp
LRUHandle* Lookup(const Slice& key, uint32_t hash) { return *FindPointer(key, hash); }
```
#### HandleTable::Remove(const Slice& key, uint32_t hash)

```cpp
LRUHandle* Remove(const Slice& key, uint32_t hash) {
    // 找到 key 对应的 LRUHandle* 在 Hash 表中的位置。
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
        // 如果找到了，那么需要将该 LRUHandle* 从 Hash 表中移除，
        // 并且更新哈希表的元素个数。
        *ptr = result->next_hash;
        --elems_;
    }
    return result;
}
```

#### HandleTable::FindPointer(const Slice& key, uint32_t hash)

`FindPointer`是`HandleTable`的核心方法，用于查找`key`对应的`LRUHandle*`在哈希表中的位置。

```cpp
LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    // key 的 hash 值模上哈希表的长度，得到 key 在哈希表中的位置。
    // 这个位置其实是哈希冲突链表的头节点，遍历这个冲突链表，找到
    // key 对应的 LRUHandle*。
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
        ptr = &(*ptr)->next_hash;
    }
    return ptr;
}
```

#### HandleTable::Resize()

往哈希表中插入新元素后，如果哈希表的元素个数超过了哈希表的长度，那么需要对哈希表进行扩容。

创建一个新哈希表，大小是老哈希表的两倍，然后将老哈希表中的所有元素逐一 hash 到新哈希表中。最后销毁掉老哈希表，用新哈希表替换掉老哈希表。

```cpp
void Resize() {
    // 哈希表扩容后的最小长度是 4
    uint32_t new_length = 4;
    // 将哈希表的长度以指数增长的方式扩大，
    // 一直扩大到可以容纳下哈希表里的所有
    // 元素为止。　
    while (new_length < elems_) {
        new_length *= 2;
    }

    // 创建一张新哈希表，将老哈希表里的所有元素逐一 hash 到新哈希表中。
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    for (uint32_t i = 0; i < length_; i++) {
        LRUHandle* h = list_[i];
        while (h != nullptr) {
            // 如果存在 hash 冲突，那么将冲突的 LRUHandle* 插入到冲突链表的尾部。
            LRUHandle* next = h->next_hash;
            uint32_t hash = h->hash;
            LRUHandle** ptr = &new_list[hash & (new_length - 1)];
            h->next_hash = *ptr;
            *ptr = h;
            h = next;
            count++;
        }
    }
    assert(elems_ == count);
    // 销毁老哈希表，用新哈希表替换掉老哈希表。
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
}
```

### LRUCache::Insert(const Slice& key, uint32_t hash, void* value, size_t charge, void (\*deleter)(const Slice& key, void* value))

```cpp
Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value, size_t charge,
                                void (*deleter)(const Slice& key, void* value)) {
    MutexLock l(&mutex_);

    // 构造一个 LRUHandle 节点
    LRUHandle* e = reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
    e->value = value;
    e->deleter = deleter;
    e->charge = charge;
    e->key_length = key.size();
    e->hash = hash;
    e->in_cache = false;
    // 提前把引用计数先加一，因为 Insert 结束后需要把创建出来的 LRUHandle 地址
    // 返回给客户端，客户端对该 LRUHandle 的引用需要加一。
    e->refs = 1;  // 
    std::memcpy(e->key_data, key.data(), key.size());

    // 如果打开数据库时配置了禁止使用 Cache，则创建出来的 Cache Capacity 就会是 0。
    if (capacity_ > 0) {
        // 这里的引用计数加一表示该 LRUHandle 在 Cache 中，是 Cache 对 LRUHandle
        // 的引用。
        e->refs++;  // 
        e->in_cache = true;
        // 把 LRUHandle 节点按照 LRU 的策略插入到 in_use_ 链表中。
        LRU_Append(&in_use_, e);
        usage_ += charge;
        // 把 LRUHandle 节点插入到 Hash 表中。
        // 如果存在相同 key 的缓存项，那么`table_.Insert(e)`会返回老的缓存项。
        // 如果存在老的缓存项，那么需要将老的缓存项从 Cache 中移除。
        FinishErase(table_.Insert(e));
    } else {
        // capacity_ == 0 表示禁止使用 Cache，所以这里不需要把 LRUHandle 节点插入到
        // 链表中。
        e->next = nullptr;
    }

    // 如果插入新的 LRUHandle 节点后，Cache 的总大小超过了容量，那么需要将最老的
    // LRUHandle 节点移除，直到 Cache 的总大小不溢出容量。
    while (usage_ > capacity_ && lru_.next != &lru_) {
        // +->oldest <-> youngest <-> lru_<-+
        // +--------------------------------+
        LRUHandle* old = lru_.next;
        assert(old->refs == 1);
        bool erased = FinishErase(table_.Remove(old->key(), old->hash));
        if (!erased) {  // 防止编译报 Warning: unused variable
            assert(erased);
        }
    }

    return reinterpret_cast<Cache::Handle*>(e);
}
```

### LRUCache::Lookup(const Slice& key, uint32_t hash)

```cpp
Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
    MutexLock l(&mutex_);
    // 到 Hash 表中查找 key 对应的缓存项指针。
    LRUHandle* e = table_.Lookup(key, hash);

    // 如果找到了缓存项，那么需要将缓存项的引用次数加一，
    // 然后返回该缓存项指针。
    if (e != nullptr) {
        Ref(e);
    }
    return reinterpret_cast<Cache::Handle*>(e);
}
```

### LRUCache::Release(Cache::Handle* handle)

```cpp
void LRUCache::Release(Cache::Handle* handle) {
    MutexLock l(&mutex_);
    // 将缓存项的引用次数减一。
    Unref(reinterpret_cast<LRUHandle*>(handle));
}
```

`Unref(LRUHandle*)`的实现如下:

```cpp
void LRUCache::Unref(LRUHandle* e) {
    assert(e->refs > 0);
    // 将缓存项的引用次数减一。
    e->refs--;
    if (e->refs == 0) {  // Deallocate.
        // 如果引用计数减少后为 0，调用 deleter 销毁该缓存项。
        assert(!e->in_cache);
        (*e->deleter)(e->key(), e->value);
        free(e);
    } else if (e->in_cache && e->refs == 1) {
        // No longer in use; move to lru_ list.
        //
        // 如果引用计数减少后为 1，表示该缓存项已经没有正在使用的客户端了，
        // 那么需要将该缓存项从 in_use_ 链表中移除，然后插入回 lru_ 链表中。
        LRU_Remove(e);
        LRU_Append(&lru_, e);
    }
}
```

`LRU_Remove(e)`的含义是把`e`从所在链表移除。如果`e`在`in_use_`链表中，那么就从`in_use_`链表中移除，如果`e`在`lru_`链表中，那么就从`lru_`链表中移除。

### LRUCache::Erase(const Slice& key, uint32_t hash)

```cpp
void LRUCache::Erase(const Slice& key, uint32_t hash) {
    MutexLock l(&mutex_);
    // 先从 Hash 表中移除 key 对应的缓存项，然后调用 FinishErase
    // 将缓存项从 Cache 中移除。
    FinishErase(table_.Remove(key, hash));
}
```

`FinishErase(LRUHandle*)`的实现如下:

```cpp
bool LRUCache::FinishErase(LRUHandle* e) {
    if (e != nullptr) {
        assert(e->in_cache);
        // 将缓存项 e 从 in_use_ 或 lru_ 链表中移除。
        LRU_Remove(e);
        e->in_cache = false;
        usage_ -= e->charge;
        // 将引用计数减一，如果减一后为零，则销毁该缓存项。
        Unref(e);
    }
    return e != nullptr;
}
```

`LRUCache::FinishErase(e)`和`LRUCache::Erase(key, hash)`的不同之处是:

- `LRUCache::FinishErase(e)`不负责将 e 从 table_ 中移除， 
- `LRUCache::Erase(key, hash)`负责。



### LRUCache::Prune()

```cpp
void LRUCache::Prune() {
    MutexLock l(&mutex_);
    // 遍历 lru_ 链表，将该链表上的所有缓存项从 Cache 中移除。
    while (lru_.next != &lru_) {
        LRUHandle* e = lru_.next;
        assert(e->refs == 1);
        bool erased = FinishErase(table_.Remove(e->key(), e->hash));
        if (!erased) {
            assert(erased);
        }
    }
}
```