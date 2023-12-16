# FilterBlockBuilder

与 BlockBuilder 类似，FilterBlockBuilder 是 FilterBlock 的构建器。

老规矩，先看下 FilterBlockBuilder 的定义，对外提供了哪些接口。

在构造 SST 的过程中，每当一个 Data Block 构建完成，就会通过`StartBlock(uint64_t block_offset)`方法在 Filter Block 里构建与下一个 Data Block 对应的 Filte。

通过`Add(const Slice& key)`方法，我们可以将需要 Key 添加到对应的 Filter 里。

最后再通过`Finish()`方法，获取 FilterBlock 的完整内容。

```c++
class FilterBlockBuilder {
   public:
    explicit FilterBlockBuilder(const FilterPolicy*);

    FilterBlockBuilder(const FilterBlockBuilder&) = delete;
    FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

    // 名字有点误导性，真实职责是构造与 Data Block 对应的 Filters。
    // block_offset 是对应的 Data Block 的起始地址。
    void StartBlock(uint64_t block_offset);

    // 将 Key 添加到 filter 里。
    void AddKey(const Slice& key);

    // 结束 Filter Block 的构建，并返回 Filter Block 的完整内容
    Slice Finish();
};
```

## FilterBlockBuilder 的代码实现

### FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy*)

先来看下 FilterBlockBuilder 的构造函数，它接收一个 FilterPolicy*，这个 FilterPolicy* 是一个接口，它定义了 Filter 的一些操作，比如`CreateFilter()`，`KeyMayMatch()`等。 关于 FilterPolicy 的详情，感兴趣的同学可以移步[TODO](TODO)。

FilterBlockBuilder 的构造函数啥也没做，只是将 FilterPolicy* 保存到成员变量里，供后续使用。

```c++
FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy) : policy_(policy) {}
```

### FilterBlockBuilder::StartBlock(uint64_t block_offset)

block_offset 指的是对应的 Data Block 的起始地址。

`FilterBlockBuilder::StartBlock`用于为下一个 Data Block 构建新的 Filter。

FilterBlockBuilder 里有个 buffer，用于积攒需要添加到 Filter 里的 Key。

`FilterBlockBuilder::StartBlock`的职责是把之前积攒的 Key 都构造成 Filter(因为这些 Key 对应的是之前的 Data Block)，然后清空 buffer，为下一个 Data Block 构建新的 Filter。

```c++
void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
    // 每个 Filter 的大小是固定的，kFilterBase，默认为 2KB。
    // Filter 与 Data Block 是分组对应的关系。
    // filter_index 用于记录当前 Data Block 对应的 Filter 的索引。
    // 比如，Data Block 的大小是 4KB，那么：
    //   - Data Block 0(block_offset = 0) 对应 Filter 0
    //   - Data Block 1(block_offset = 4KB) 对应 Filter 2
    //   - Data Block 2(block_offset = 8KB) 对应 Filter 4
    // Filter 1, 3, 5 是空的，不对应任何 Data Block。
    uint64_t filter_index = (block_offset / kFilterBase);

    assert(filter_index >= filter_offsets_.size());

    // filter_offsets_ 中记录了每个 Filter 的起始偏移量。
    // 换句话说，filter_offsets_.size() 就是已经构造好的 Filter 数量。
    // 在记录新的 Key 之前，需要先把 buffer 里积攒的 Key (属于上一个 Data Block 的 Key)都构造成 Filter。
    // 这里的 while 是上一个 Data Block 构建 Filter，然后清空 filter buffer，为新的 Data Block 做准备。
    // 这里的 while 理解起来会有点误解，看上去好像可能会为了 1 个 Data Block 构建多个 Filter 的样子，其实不是。
    // 假设 Data Block 大小为 4KB, Filter 大小为 2KB，那么 Filter 0 对应 Data Block 0，Filter 1 是个空壳，不对应任何 Data Block。
    // 假设 Data Block 的大小为 1KB，Filter 的大小为 2KB，那么就会有 1 个 Filter 对应 2 个 Data Block。
    while (filter_index > filter_offsets_.size()) {
        GenerateFilter();
    }
}
```

### FilterBlockBuilder::GenerateFilter()

`GenerateFilter()`的作用是根据 filter buffer 生成一个 filter，将该 filter 压入到 result_中，并更新 filter_offsets。

如果 filter buffer 里没有任何 Key，那么就只是往 filter_offsets_ 里压入一个位置，这个位置指向上一个 filter 的位置，不往 result_ 里压入任何 filter。

相当于构造了一个空的 dummy filter。

`GenerateFilter`的流程简化如下:

1. 获取 filter buffer 里 key 的数量
2. 如果 key 的数量为 0，那就构造一个空的 dummy filter，结束。
3. key 的数量不为 0，继续往下走。
4. 取出所有的 key，构造出一个 filter，压入到 result_ 中。
5. 清空 filter buffer，为下一个 filter 做准备。

实现如下：

```c++
// 生成一个 filter，然后压入到 result_ 中
void FilterBlockBuilder::GenerateFilter() {
    // std::vector<size_t> start_ 里存储的是每个 key 在 std::string keys_ 中的位置。
    // start_.size() 代表的是 keys_ 缓冲区中的 key 的数量。
    const size_t num_keys = start_.size();

    // std::string result_ 里存放的是每个 filter 拍平后的数据，也就是说，
    // filter-0, filter-1, ..., filter-n 是按顺序连续存放到 result_ 里的。
    // 
    // std::vector<uint32_t> filter_offsets_ 里则存放每个 filter 在 result_ 里的位置，
    // 比如 filter_offsets[0] 代表的是 filter-0 在 result_ 里的位置。
    // 
    // 此处如果 num_keys 为 0，则构造一个空的 dummy filter，但是将该 filter 的
    // 位置 filter_offsets_[i] 记录为上一个 filter 的位置。
    // 比如 filter_offsets[1] = filter_offsets[0]，filter-0 是有实际数据的，
    // 但 filter-1 是个空的 dummy filter，就把 filter_offsets[1] 指向 filter-0。
    // 这主要用于 Data Block 的大于 Filter 大小的情况，参考 FilterBlockBuilder::StartBlock
    // 中的注释可更好理解。
    if (num_keys == 0) {
        // Fast path if there are no keys for this filter
        // 只是往 filter_offsets_ 里压入一个位置，但是没有往 result_ 里压入 filter。
        filter_offsets_.push_back(result_.size());
        return;
    }

    // 此处往 start_ 里压入下一个 key 起始位置是为了方便计算 keys_ 中最后一个 key 的长度。
    // keys_[i] 的长度计算方式为 start_[i+1] - start_[i]。
    // 假设 keys_ 里一共有 n 个 key，那么 start_ 里一共有 n 个元素，如果我们要计算
    // 最后一个 key 的长度，则为 start_[n] - start[n-1]，但是 start_[n] 并不存在，越界了。
    // 所以这里先压入一个 start_[n]，就可以计算出最后一个 key 的长度了。
    // 反正 GenerateFilter() 结束后 start_ 就会被清空重置了，所以这里修改 start_ 没有问题。
    start_.push_back(keys_.size());  

    /* tmp_keys_ 主要的作用就是作为 CreateFilter() 方法参数构建 Bloom Filter */
    // tmp_keys_ 只是一个临时的富足
    tmp_keys_.resize(num_keys);

    // 从 keys_ 中取出所有的 key，放到 std::vector<Slice> tmp_keys_ 中。
    // 不太懂为什么 leveldb 不直接把 keys_ 设为 std::vector<Slice> 类型 = =。
    for (size_t i = 0; i < num_keys; i++) {
        // 取 keys_[i] 的起始地址
        const char* base = keys_.data() + start_[i]; 
        // 计算 keys[i] 的长度，
        // 现在可以看到上面 start_.push_back(keys_.size()) 的作用了
        size_t length = start_[i + 1] - start_[i];   
        // 把 keys_[i] 放到 tmp_keys_[i] 中
        tmp_keys_[i] = Slice(base, length);          
    }

    // 先把新 filter 的位置记录到 filter_offsets_ 中
    filter_offsets_.push_back(result_.size());
    // 再构造新 filter，然后压入到 result_ 中
    policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

    // 新 filter 构造完毕，把缓冲区清空，为下一个 filter 做准备。
    tmp_keys_.clear();
    keys_.clear();
    start_.clear();
}
```

细心的同学可能会有疑惑，`FilterBlockBuilder::tmp_keys_`只是在 `FilterBlockBuilder::GenerateFilter()`这一个方法中有被使用到，其作用只是一个临时的局部变量而已，`FilterBlockBuilder`为什么要把`tmp_keys_`设为成员变量？

其实是为了性能。`tmp_keys_`是`std::vector`类型，如果设为局部变量，那么每次调用`GenerateFilter()`时都需要重新创建`tmp_keys`并且重新分配内存。

假设每次调用`FilterBlockBuilder::GenerateFilter()`时使用到的`tmp_keys_`的内存空间为 4KB，那每次调用`GenerateFilter()`都需要重新创建`tmp_keys_`，并重新分配 4KB 的内存。

而如果将`tmp_keys_`设为成员变量，那么只需要在第一次调用`GenerateFilter()`时创建`tmp_keys_`，并分配 4KB 的内存，这 4KB 的内存会一直保留，直到`FilterBlockerBuilder`对象销毁。