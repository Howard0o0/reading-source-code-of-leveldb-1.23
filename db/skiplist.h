// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_SKIPLIST_H_
#define STORAGE_LEVELDB_DB_SKIPLIST_H_

// Thread safety
// -------------
//
// Writes require external synchronization, most likely a mutex.
// Reads require a guarantee that the SkipList will not be destroyed
// while the read is in progress.  Apart from that, reads progress
// without any internal locking or synchronization.
//
// Invariants:
//
// (1) Allocated nodes are never deleted until the SkipList is
// destroyed.  This is trivially guaranteed by the code since we
// never delete any skip list nodes.
//
// (2) The contents of a Node except for the next/prev pointers are
// immutable after the Node has been linked into the SkipList.
// Only Insert() modifies the list, and it is careful to initialize
// a node and use release-stores to publish the nodes in one or
// more lists.
//
// ... prev vs. next pointer ordering ...

#include <atomic>
#include <cassert>
#include <cstdlib>

#include "util/arena.h"
#include "util/random.h"

namespace leveldb {

class Arena;

/*
 * SkipList 属于 leveldb 中的核心数据结构，也是 memory table 的具体实现
 *
 * SkipList 的实现挺有意思的，leveldb 是一个 key-value DB，但是 SkipList
 * 类中只定义了 Key， 而没有定义 value。这是为什么?
 *
 * 因为 leveldb 直接将 User Key 和 User Value 打包成了一个更大的 Key，塞到了
 * Skip List 中。
 *
 * ┌───────────────┬─────────────────┬────────────────────────────┬───────────────┬───────────────┐
 * │ size(varint32)│ User Key(string)│Sequence Number | kValueType│
 * size(varint32)│  User Value   │
 * └───────────────┴─────────────────┴────────────────────────────┴───────────────┴───────────────┘
 *        ↑
 *  值为 user_key.size() + 8
 */
template <typename Key, class Comparator>
class SkipList {
   private:
    struct Node;

   public:
    // Create a new SkipList object that will use "cmp" for comparing keys,
    // and will allocate memory using "*arena".  Objects allocated in the arena
    // must remain allocated for the lifetime of the skiplist object.
    explicit SkipList(Comparator cmp, Arena* arena);

    // 不允许进行拷贝构造与拷贝赋值
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Insert key into the list.
    // REQUIRES: nothing that compares equal to key is currently in the list.
    void Insert(const Key& key);

    // Returns true iff an entry that compares equal to key is in the list.
    // (iff, if and only if)
    bool Contains(const Key& key) const;

    // Iteration over the contents of a skip list
    class Iterator {
       public:
        // Initialize an iterator over the specified list.
        // The returned iterator is not valid.
        // 传入一个skiplist即可构造一个Iterator
        explicit Iterator(const SkipList* list);

        // Returns true iff the iterator is positioned at a valid node.
        // 判断当前迭代器是否有效,
        // 等效于c++标准库里的`it != end()`
        bool Valid() const;

        // Returns the key at the current position.
        // REQUIRES: Valid()
        // 返回当前迭代器所指向的节点的key
        const Key& key() const;

        // Advances to the next position.
        // REQUIRES: Valid()
        // 将迭代器指向下一个节点,
        // 等效于c++标准库里的`it++`
        void Next();

        // Advances to the previous position.
        // REQUIRES: Valid()
        // 将迭代器指向前一个节点,
        // 等效于c++标准库里的`it--`
        void Prev();

        // Advance to the first entry with a key >= target
        // 查找第一个大于等于target的节点,
        // 并将迭代器指向该节点
        void Seek(const Key& target);

        // Position at the first entry in list.
        // Final state of iterator is Valid() iff list is not empty.
        // 将迭代器指向第一个节点,
        // 等效于c++标准库里的`it = begin()`
        void SeekToFirst();

        // Position at the last entry in list.
        // Final state of iterator is Valid() iff list is not empty.
        // 将迭代器指向最后一个节点,
        // 等效于c++标准库里的`it = rbegin()`
        void SeekToLast();

       private:
        const SkipList* list_;
        Node* node_;
        // Intentionally copyable
    };

   private:
    // 经验值
    enum { kMaxHeight = 12 };

    inline int GetMaxHeight() const {
        // 简单的原子性取出层高，无所谓指令重排
        return max_height_.load(std::memory_order_relaxed);
    }

    Node* NewNode(const Key& key, int height);

    // TODO
    // Write a UT to test the probability distribution.
    int RandomHeight();

    bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

    // Return true if key is greater than the data stored in "n"
    bool KeyIsAfterNode(const Key& key, Node* n) const;

    // Return the earliest node that comes at or after key.
    // Return nullptr if there is no such node.
    //
    // If prev is non-null, fills prev[level] with pointer to previous
    // node at "level" for every level in [0..max_height_-1].
    Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

    // Return the latest node with a key < key.
    // Return head_ if there is no such node.
    Node* FindLessThan(const Key& key) const;

    // Return the last node in the list.
    // Return head_ if list is empty.
    Node* FindLast() const;

    // Immutable after construction
    // 比较器
    Comparator const compare_;

    // leveldb 自己封装的一个内存分配器
    Arena* const arena_;  // Arena used for allocations of nodes

    // 虚拟头结点，也就是 Dummy Head
    Node* const head_;

    // Modified only by Insert().  Read racily by readers, but stale
    // values are ok.
    // 原子变量的层高
    std::atomic<int> max_height_;  // Height of the entire list

    // Read/written only by Insert().
    Random rnd_;
};

// Implementation details follow
/*
 * Node 中使用了比较多的关于指令重排的内容。
 *
 * 需要注意的是，memory ordering
 * 是针对于单线程而来的，也就是同一个线程内的指令重排情况，比如 现在有 2 条语句:
 *
 *  x = 100;
 *  y.store();
 *
 * 其中 x 的写入是非原子性的，而 y 的写入是原子性的，不管我们使用何种 memory
 * ordering，y 的原子 写入永远是满足的，也就是说，y.store()
 * 必然是多个线程的一个同步点。但是，由于指令重排的原因，x = 100; 可能会在
 * y.store(); 之后执行，也可能会在其之前执行。memory ordering 限制的就是这个。
 *
 * 1. Relaxed ordering
 *
 * Relaxed ordering，也就是
 * std::memory_order_relaxed，不对重排进行任何限制，只保证相关内存操作的原子性。
 * 原子操作之前或者是之后的指令怎么被重排，我们并不关心，反正保证对内存的操作是原子性的就行了。通常用于计数器等场景中
 *
 *
 * 2. Release-Acquire ordering
 *
 * Release-Acquire ordering 由两个参数所指定，一个是
 * std::memory_order_acquire，用于 load() 方法， 一个则是
 * std::memory_order_release， 用于 store() 方法。
 *
 * std::memory_order_acquire 表示在 load()
 * 之后的所有读写操作，不允许被重排到这个 load() 的前面。
 * std::memory_order_release 表示在 store()
 * 之前的所有读写操作，不允许被重排到这个 store() 的后面
 */
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
    explicit Node(const Key& k) : key(k) {}

    Key const key;

    // Accessors/mutators for links.  Wrapped in methods so we can
    // add the appropriate barriers as necessary.
    // 获取第n层的下一个节点
    // 带Barrier的版本
    Node* Next(int n) {
        assert(n >= 0);
        // Use an 'acquire load' so that we observe a fully initialized
        // version of the returned Node.
        return next_[n].load(std::memory_order_acquire);
    }

    // 设置第n层的下一个节点为x
    void SetNext(int n, Node* x) {
        assert(n >= 0);
        // Use a 'release store' so that anybody who reads through this
        // pointer observes a fully initialized version of the inserted node.
        next_[n].store(x, std::memory_order_release);
    }

    // No-barrier variants that can be safely used in a few locations.
    Node* NoBarrier_Next(int n) {
        assert(n >= 0);
        return next_[n].load(std::memory_order_relaxed);
    }
    void NoBarrier_SetNext(int n, Node* x) {
        assert(n >= 0);
        next_[n].store(x, std::memory_order_relaxed);
    }

   private:
    // Array of length equal to the node height.  next_[0] is lowest level link.
    // 1) 这里提前声明并申请了一个内存，用于存储第 0 层的数据，因为第 0
    // 层必然存在数据。 2) 这里的数组长度其实就是层高，假设 next_ 长度为
    // n，那么就会从 next_[n-1] 开始查找。 3) 因为 skip list 的 level
    // 并不会太大，使用数组存储 Node 指针的话对 CPU 内存更友好
    // https://15721.courses.cs.cmu.edu/spring2018/papers/08-oltpindexes1/pugh-skiplists-cacm1990.pdf
    std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                             int height) {
    // 内存分配时只需要再分配 level - 1 层，因为第 0 层已经预先分配完毕了。
    // 一共需要分配 height 个next_指针。
    // sizeof(Node) 分配的是struct Node的大小，其中包含了1个next_指针
    // sizeof(std::atomic<Node*>) * (height - 1)) 分配 height-1 个next_指针
    char* const node_memory =
        arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
    // 这里是 placement new 的写法，在现有的内存上进行 new object
    return new (node_memory) Node(key);
}

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) {
    // 保存skiplist的指针,
    // 后续的操作都是基于这个指针进行的.
    list_ = list;

    // 将当前节点指针指向一个非法地方
    node_ = nullptr;
}

template <typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const {
    return node_ != nullptr;
}

template <typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const {
    assert(Valid());
    return node_->key;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next() {
    assert(Valid());
    node_ = node_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
    // Instead of using explicit "prev" links, we just search for the
    // last node that falls before key.
    assert(Valid());
    node_ = list_->FindLessThan(node_->key);
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
    node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
    node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
    node_ = list_->FindLast();
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
    // 概率因子p = 1/kBranching = 1/4.
    static const unsigned int kBranching = 4;

    // 从1开始抛硬币
    int height = 1;
    // (rnd_.Next() % kBranching) == 0
    // 这个条件限制了上层的节点数量为下层节点数量的 1/4
    while (height < kMaxHeight && ((rnd_.Next() % kBranching) == 0)) {
        // rnd_.Next()生成一个随机数,
        // rnd_.Next() % 4的意思是, 生成一个0~3的随机数,
        // 0,1,2,3的概率都是1/4.
        // 所以(rnd_.Next() % 4) == 0成立的概率是1/4.
        // 也就是说每次抛硬币都有1/4的概率层高+1.
        // 所以LevelDB的SkipList里, 概率因子是1/4.
        height++;
    }

    // 生成的height必须在[1, kMaxHeight]之间
    assert(height > 0);
    assert(height <= kMaxHeight);

    return height;
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
    // null n is considered infinite
    return (n != nullptr) && (compare_(n->key, key) < 0);
}

/* 在 Skip List 中寻找第一个大于等于 key 的节点，同时使用 prev
 * 数组记录下该节点的每一个 level 的前驱节点，用于辅助实现 insert 和 delete
 * 操作，把 prev 数组当作是单向链表的 prev 节点就可以了
 *
 *    5->10->18->22->35->44
 *           ↑   ↑
 *         prev node
 *
 * 如上面的单链表，node
 * 是我们要查找的节点，当我们返回给调用方之后，如果调用方需要做删除操作的话，
 * 就可以这样来做:
 *
 *    prev->next = node->next;
 *    node->next = nullptr;
 *    delete node;
 *
 * 很好的一个设计，在查找的过程中记录一些其他接口所需的信息，最大可能地进行代码复用。
 * 接口设计的很好, 当传入的prev不为null时, 会将每一层的前驱节点都记录下来,
 * 便于代码复用.
 */
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
    // x为查找目标节点
    Node* x = head_;

    // index是从0开始的，所以需要减去1
    int level = GetMaxHeight() - 1;
    while (true) {
        // 获取当前level层的下一个节点
        Node* next = x->Next(level);

        // KeyIsAfterNode实际上就是使用 Compactor 比较 Key 和 next->key
        //      key > next->key:  return true
        //      key <= next->key: return false
        if (KeyIsAfterNode(key, next)) {
            // 待查找节点比next->key
            // 还要大的，那么就继续在同一层向后查找
            x = next;
        } else {
            // 当前待查找节点比next-key小,
            // 需要往下一层查找.

            // prev 数组主要记录的就是每一层的 prev
            // 节点，主要用于插入和删除时使用
            if (prev != nullptr) prev[level] = x;

            // 如果当前层已经是最底层了，没法再往下查找了，
            // 则返回当前节点
            if (level == 0) {
                return next;
            } else {
                // 还没到最底层, 继续往下一层查找
                level--;
            }
        }
    }
}

/* 寻找最后一个小于等于 key 的节点 */
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLessThan(
    const Key& key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        assert(x == head_ || compare_(x->key, key) < 0);
        Node* next = x->Next(level);
        if (next == nullptr || compare_(next->key, key) >= 0) {
            if (level == 0) {
                return x;
            } else {
                // Switch to next list
                level--;
            }
        } else {
            x = next;
        }
    }
}

/* 获取 Skip List 中的最后一个节点。注意 FindLast() 实现不能从 level 0 直接使用
 * next 指针 一路往前寻找，因为这样的话其时间复杂度将为
 * O(n)。而从最高层往下找的话，其时间复杂度为 O(logn) */
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
    Node* x = head_;
    // 从最高层找起, level的取值是[0, Height - 1].
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (next == nullptr) {
            if (level == 0) {
                // 如果next为nullptr, 且level已经是最底层了, 说明已经是level-0的最后一个节点了,
                // 也就是我们的目标节点, return
                return x;
            } else {
                // 如果next为nullptr, 但是level还没到最底层, 就降一层
                level--;
            }
        } else {
            // 当前层还没有到最后一个节点, 继续往后找
            x = next;
        }
    }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(0 /* any key will do */, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
    for (int i = 0; i < kMaxHeight; i++) {
        head_->SetNext(i, nullptr);
    }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
    // TODO(opt): We can use a barrier-free variant of FindGreaterOrEqual()
    // here since Insert() is externally synchronized.
    // prev是待插入节点的前驱节点
    // 将prev声明为kMaxHeight层, 多出来的不用
    Node* prev[kMaxHeight];

    // 找到前驱节点
    Node* x = FindGreaterOrEqual(key, prev);

    // Our data structure does not allow duplicate insertion
    // 如果发现key已经存在于SkipList中了, 那是有问题的.
    // 因为key = sequence + key + value.
    // 就算key相同, sequence是全局递增的, 不会重复
    // 使用assert是为了在debug模式下与ut中测试,
    // 但是在release模式中, 会被编译器优化掉, 不生效,
    // 同时也增加了可读性.
    assert(x == nullptr || !Equal(key, x->key));

    // 给新节点按概率随机生成一个层高
    int height = RandomHeight();

    // 如果新节点的层高比SkipList的当前层高还要大, 那么就需要做些更新
    if (height > GetMaxHeight()) {
        // 假设SkipList的当前层高是4, 新节点的层高是6,
        // 那么第5层和第6层的前驱节点都是head(DummyHead)
        for (int i = GetMaxHeight(); i < height; i++) {
            prev[i] = head_;
        }

        // It is ok to mutate max_height_ without any synchronization
        // with concurrent readers.  A concurrent reader that observes
        // the new value of max_height_ will see either the old value of
        // new level pointers from head_ (nullptr), or a new value set in
        // the loop below.  In the former case the reader will
        // immediately drop to the next level since nullptr sorts after all
        // keys.  In the latter case the reader will use the new node.
        // 原子更新SkipList的当前层高
        max_height_.store(height, std::memory_order_relaxed);
    }

    // 创建新节点
    x = NewNode(key, height);

    // 借助前驱节点prev将新节点插入到SkipList中
    for (int i = 0; i < height; i++) {
        // NoBarrier_SetNext() suffices since we will add a barrier when
        // we publish a pointer to "x" in prev[i].

        // NoBarrier_SetNext()使用的是std::memory_order_relaxed.
        // SetNext使用的是std::memory_order_release.
        // 之所以使用NoBarrier_SetNext是因为后面还有个std::memory_order_release,
        // 保证x->NoBarrier_SetNext不会重排到prev[i]->SetNext之后.
        // 后面会详细讲解内存屏障与指令重排的关系.
        x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
        prev[i]->SetNext(i, x);
    }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    if (x != nullptr && Equal(key, x->key)) {
        return true;
    } else {
        return false;
    }
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_SKIPLIST_H_
