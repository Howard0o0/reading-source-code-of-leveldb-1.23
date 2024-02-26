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

`lru_`链表的尾部的节点，是访问时间最新的节点，而越靠近头部的节点，访问时间越早。

当需要往`Cache`中插入新节点的时候，会将该节点插入到`lru_`链表的尾部。

如果`Cache`满了，需要移除一些老节点为新节点腾出空间，就会从`lru_`链表的头部开始移除节点，直到空间足够插入新节点位置。

这样一来，往`Cache`中插入新节点就解决了，只需要$O(1)$的时间从`lru_`链表的尾部插入即可。

那么怎么从`Cache`中快速查找一个缓存项呢？这就需要用到`table_`哈希表了。

### table_ 哈希表

`table_`哈希表是一个`HandleTable`，用于快速查找`Cache`中的缓存项。

LevelDB 设计的`LRUHandle`很巧妙，`LRUHandle`中存储了该缓存项的`key`、`value`、`hash`等信息。

其中`key`是这个缓存项的唯一标识，`hash`是`key`的哈希值，`value`是缓存项的值。

使用`table_.Lookup(key, hash)`可以在$O(1)$的时间复杂度内查找到`key`对应的缓存项在`lru_`链表中的位置。

也就是说，往`Cache`中插入一个`Key-Value`时，会构建出一个`LRUHandle`插入到`lru_`链表的尾部，同时会在`table_`哈希表中插入`{key, LRUHandle}`。

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

所以，`in_use_`链表的作用只是让我们能清晰的知道哪些缓存项是正在被客户端使用的，哪些是在`Cache`中但是没有正在被使用。如果把`in_use_`链表去掉，`LRUCache`的功能也不会受到影响，还是能正常工作。

