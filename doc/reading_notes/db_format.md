@[TOC]
# LevelDB中的数据格式

## Key

先说LevelDB里比较容易混淆的3种Key:

- UserKey
- InternalKey
- LookupKey

这三种Key是包含的关系。

[[LevelDB] 数据库2：所见非所得 —— 三种Key](https://zhuanlan.zhihu.com/p/272468157)，这篇文章把这三种Key讲的很清楚，在此简洁的概括一下。

![3种Key的关系][https://pic2.zhimg.com/80/v2-173f1ad63cc3191dec7befc0037413bd_1440w.webp]

### UserKey

最简单的key，调用`db->Put(key, value)`时传入的key。

### InternalKey

存储在`SSTable`中的key。

`InternalKey` = `UserKey` + `SequenceNumber` + `ValueType`

`SequenceNumber`是7B的UUID。

`ValueType`是为了区分一个Key是插入还是删除，取值如下：

- kTypeDeletion
- kTypeValue
### LookupKey

`LookupKey` = `InternalKey_Len` + `InternalKey`

`InternalKey_Len`是`Varint`类型的。什么是`Varint`，移步阅读[大白话解析LevelDB：整数序列化](https://blog.csdn.net/sinat_38293503/article/details/134736576?spm=1001.2014.3001.5502#Varint_119)。

## WriteBatch

`WriteBatch`本质是一个`std::string`, `rep_`是数据本身, `WriteBatch`封装了一些方法来对`rep_`的内容进行插入和读取.
`rep_`的格式如下：

```
|<---- 8 bytes ---->|<-- 4 bytes -->|<--------- Variable Size -------->|
+-------------------+---------------+----------------------------------+
|    Sequence       |     Count     |              Records             |
+-------------------+---------------+----------------------------------+
```

`Sequence`是一个64位的整数。在LevelDB 中，`Sequence`是一个非常关键的概念，用于实现其存储和检索机制。每个存储在 LevelDB 中的键值对都与一个唯一的序列号（sequence number）相关联。序列号的作用和重要性可以从以下几个方面理解：

- 版本控制
    - LevelDB 支持多版本并发控制（MVCC），即允许存储同一个键的多个版本。
    - 序列号用于标记同一个键在不同时间点的值。当存储相同键的新数据时，LevelDB 会分配一个新的序列号，这使得LevelDB能够保留同一键的历史版本。
- 读写隔离
    - 在并发环境中，LevelDB 使用序列号来保证读写操作的隔离性。
    - 通过检查序列号，LevelDB 能够为读操作提供一个一致的视图，即使在有其他写入操作发生的时候。
- 事务日志(WAL)
    - LevelDB 使用写前日志（Write-Ahead Logging，WAL）来保证数据的持久性。每个写入操作先写入日志，再写入存储结构。
    - 序列号在这里用于确保即使在系统崩溃的情况下，也能正确地恢复数据，并保持数据的一致性。
- 压缩和合并(Compaction)
    - LevelDB 定期进行压缩和合并操作，以减少存储空间的使用，并优化读取性能。
    - 在这个过程中，序列号帮助 LevelDB 确定哪些旧版本的数据可以被安全删除。
- 快照(Snapshot)
    - LevelDB 支持快照功能，允许用户获取特定时间点的数据库状态。
    - 序列号用于实现这一点，通过记录创建快照时的序列号，LevelDB 可以提供该时刻所有键的视图。

`Count`是一个32位的定长整数(`FixedInt32`)，表示`rep_`中`Records`的数量.
`FixedInt32` 编码方式在 LevelDB 中用于存储固定长度的 32 位（4 字节）整数。详情请移步[大白话解析LevelDB：整数序列化](https://blog.csdn.net/sinat_38293503/article/details/134736576)😁。


`Records`是一个序列，每个元素是一个`Record`，`Record`的格式如下：

```
|<-- 1 byte -->|<---- Variable Size ---->|<---- Variable Size ---->|
+--------------+-------------------------+-------------------------+
|   Type       |        UserKey          |        Value            |
+--------------+-------------------------+-------------------------+
```

其中`Type`是一个1字节的整数，表示`Record`的类型。`Type`的取值如下

```c++
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };
```

不知道`UserKey`是什么的移步[这里](https://editor.csdn.net/md/?not_checkout=1&spm=1001.2101.3001.5352#UserKey_17)。


## SST

在LevelDB中，SST（Sorted String Table）是一种用于存储键值对的文件格式。SST文件被设计为高效地支持范围查询和顺序读取操作。一个标准的 SST 文件包含以下主要部分：

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

**解释**:

- **Data Blocks**: 这些是文件的主体，包含了实际的键值对。每个 Data Block 存储一系列键值对。

- **Index Block**: 包含指向各个 Data Blocks 的索引。每个索引项包括一个 Data Block 的最大键和该 Data Block 的位置信息。

- **Meta Block**: 目前 LevelDB 里只有一种 Meta Block，就是 Filter Block。 所以我们就把 Meta Block 直接看成 Filter Block就行。如果使用了过滤策略（如布隆过滤器），则在这里存储相关数据。它用于快速判断一个键是否存在于某个 Data Block 中，以减少不必要的磁盘读取。

- **Meta Index Block**: Meta Index Block 包含指向各个 Meta Block的索引，每个索引项包含所对应的 Meta Block 的位置信息。

- **Footer**: 包含了指向 Index Block 和 Meta Index Block 的指针，以及一些其他元数据和一个 Magic Number，用于标识和验证 SST 文件的格式。

为什么一个 SST 里要包含多个 Data Block，而不是把所有的 Key-Vaue 放到一个 Data Block 里呢？

主要是出于性能优化的考虑：

- **读取效率**：使用多个较小的 Data Blocks 可以提高读取效率。当需要查找特定的 Key-Value 对时，只需加载包含该键的 Data Block，而不是整个文件。这减少了读取时所需的内存和磁盘 I/O 操作。

- **缓存友好性**：小的 Data Blocks 更加适合于缓存。在 LevelDB 中，常用的 Data Blocks 可以被缓存在内存中，从而加速读取操作。较小的 Block 尺寸意味着可以在有限的缓存空间中存储更多不同的 Blocks，从而提高缓存命中率。

- **Bloom 过滤器的效果**：LevelDB 中的 Bloom 过滤器用于快速判断一个键是否不在某个 Data Block 中，以避免不必要的磁盘读取。较小的 Data Blocks 意味着更精确的 Bloom 过滤器，因为每个过滤器只需要涵盖较小范围内的键。

- **并发和写入放大**：将数据分散到多个 Blocks 中可以减少写入放大（写入数据量大于实际数据量的现象）和提高并发写入性能。在数据更新或合并时，只需重写受影响的 Blocks 而非整个文件。


### Data Block

Data Block 是存储实际键值对的基本单位。每个 Data Block 包含了一系列的键值对，这些键值对是根据键进行排序的。下面是一个简化的示意图，描述了 Data Block 的结构：

```
+----------------------------------+
| Key1   | Value1                  |
+----------------------------------+
| Key2   | Value2                  |
+----------------------------------+
| Key3   | Value3                  |
+----------------------------------+
| ............                     |
+----------------------------------+
| KeyN   | ValueN                  |
+----------------------------------+
| Block Trailer (压缩信息，校验码等)  |
+----------------------------------+
```

**解释**:

- **键值对**：Data Block 的主体部分是一系列的键值对，这些键值对按键的字典顺序存储。每个键值对包括一个键（Key）和一个值（Value），此处的 Key 为 Internal Key。

- **Block Trailer**：Data Block 的末尾通常包含一个 Block Trailer，用于存储关于这个 Data Block 的额外信息。这可能包括压缩信息（如果 Data Block 被压缩了的话），校验码（用于检测数据损坏或错误）等。

- **键的排序**：在 Data Block 内，键是根据字典顺序进行排序的。这种排序使得基于范围的查询更加高效。

- **Block 大小**：LevelDB 允许配置 Data Block 的大小，通常大小设置在几千到几万字节之间。较小的 Block 可以提高缓存效率，而较大的 Block 可能在读取时减少磁盘 I/O。

- **压缩**：LevelDB 支持对 Data Blocks 使用压缩算法（如 Snappy 压缩），以减少存储空间的使用和提高 I/O 效率。

- **读取过程**：当需要从 SST 文件中查找一个键时，LevelDB 首先使用 Index Block 来找到包含该键的 Data Block，然后再到该 Data Block 中进行搜索。

### Index Block

Index Block 用于存储指向各个 Data Blocks 的索引信息。这个结构使得查找特定键值对时能够快速定位到包含这个键的 Data Block。以下是 Index Block 的一个简化示意图和相关解释：

```
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

**解释**:

- **键（Key）**：每个索引项包含一个键，这个键通常是对应 Data Block 中最大的键（或一个稍大于该 Data Block 中最大键的最小键，也称为分隔符）。这些键用于在查询时快速定位相应的 Data Block。

- **Block Handle**：每个索引项还包含一个 Block Handle，它是一个包含 Data Block 位置信息的数据结构。Block Handle 通常包含该 Data Block 在 SST 文件中的起始偏移量和大小。

- **索引的顺序**：Index Block 中的索引项按键的顺序排列，这与 Data Blocks 中的键顺序相一致。

- **查找过程**：在查找一个特定的键时，LevelDB 首先在 Index Block 中进行二分查找，以确定包含该键的 Data Block。找到相应的 Block Handle 后，LevelDB 读取对应的 Data Block 并在其中搜索该键。

- **优化读取性能**：由于 Index Block 相对较小，它通常可以被完整地加载到内存中，这样可以快速进行索引查找，从而显著提高整体读取性能。

- **压缩和缓存**：与 Data Blocks 一样，Index Blocks 也可以被压缩以节省存储空间，并且常常被缓存以加速访问。

### Meta Block (Filter Block)

Meta Block 的格式如下：

```plaintext
+---------------------+
|   Meta Block Data   |
+---------------------+
|   Block Trailer     |
+---------------------+

```

**解释**:

- **Meta Block Data**:
   - 这部分包含了 Meta Block 的实际内容，比如布隆过滤器的数据或其他类型的元数据。
   - 具体内容取决于 Meta Block 的类型，比如 "filter" 类型的 Meta Block 将包含布隆过滤器的序列化数据。

- **Block Trailer**:
   - Block Trailer 是 Meta Block 的尾部，包含用于验证和管理 Meta Block 的额外信息。
   - 通常包括压缩信息（如果 Meta Block 被压缩的话）和校验码（用于检测数据损坏或错误）。

目前 LevelDB 里，Meta Block 只有 Filter Block 一种类型，所以我们直接 Bloom Filter Block 代入 Meta Block Data 就行。

当 Filter Block 用作布隆过滤器（Bloom Filter）时，在 LevelDB 的 SST 文件中，它主要用于快速判断一个特定的键是否可能存在于某个 Data Block 中。布隆过滤器是一个高效的概率型数据结构，用于测试一个元素是否是一个集合的成员。下面是当 Filter Block 作为布隆过滤器时的具体内容和格式：

```plaintext
+-----------------------------------+
|   Bloom Filter for Data Block 1   |
+-----------------------------------+
|   Bloom Filter for Data Block 2   |
+-----------------------------------+
|               ...                 |
+-----------------------------------+
|   Bloom Filter for Data Block N   |
+-----------------------------------+
|   Offset of Bloom Filter 1 (4B)   |
+-----------------------------------+
|   Offset of Bloom Filter 2 (4B)   |
+-----------------------------------+
|               ...                 |
+-----------------------------------+
|   Offset of Bloom Filter N (4B)   |
+-----------------------------------+
|   Offset Array Start (4 bytes)    |
+-----------------------------------+
|   lg(Base) (1 byte)               |
+-----------------------------------+
```

**解释**:

- **布隆过滤器**:
   - 每个 "Bloom Filter for Data Block" 表示一个特定 Data Block 的布隆过滤器。
   - 它由一系列位（0或1）组成，这些位根据该 Data Block 中的键值对通过哈希函数得到。

- **偏移量**:
   - 每个布隆过滤器的末尾是一个 4 字节的偏移量，指向该过滤器在 Filter Block 中的位置。
   - 这些偏移量允许快速定位到特定 Data Block 的布隆过滤器。

- **偏移量数组的开始位置**:
   - Filter Block 的末尾包含一个 4 字节的值，表示偏移量数组开始的位置。就是 Offset of Bloom Filter 1 的位置。

- **lg(Base)**:
   - 最后是一个字节的 lg(Base) 值，用于计算布隆过滤器的大小和参数。
   - lg(Base) 实际上是 Base 的对数值。例如，如果 Base 是 2048，那么 lg(Base) 就是 11，因为 2 的 11 次幂等于 2048。Base 为 2048 意味着每个布隆过滤器的大小为 2048 位，即 256 字节。


### Meta Index Block 

在 LevelDB 的 SST 文件中，`Meta Index Block` 是一个特殊的索引块，它的主要作用是存储和索引其他 Meta Blocks 的信息。这些 Meta Blocks 通常包括诸如布隆过滤器（Bloom Filter）等用于优化数据检索的结构。`Meta Index Block` 使得 LevelDB 能够高效地定位和访问这些 Meta Blocks。

Index Block 存储的是 Data Block 的索引。类似的，Meta Index Block 存储 Meta Block 的索引。

```
+---------------------------------------------------+
| Meta Block Name 1 | Block Handle to Meta Block 1  |
+---------------------------------------------------+
| Meta Block Name 2 | Block Handle to Meta Block 2  |
+---------------------------------------------------+
|                       ...                         |
+---------------------------------------------------+
| Meta Block Name N | Block Handle to Meta Block N  |
+---------------------------------------------------+
```

**解释**：

- **索引项**:
   - 每个索引项包含两部分：Meta Block 的名称和一个 BlockHandle。
   - Meta Block 的名称是一个字符串，如 "filter"，表示对应的 Meta Block 类型。
   - BlockHandle 是一个指向实际 Meta Block 存储位置的指针，包含了该 Meta Block 在 SST 文件中的偏移量和大小。

- **功能**:
   - `Meta Index Block` 使 LevelDB 能够快速定位到特定类型的 Meta Block，例如快速找到布隆过滤器的数据块。
   - 当 LevelDB 执行读取操作时，它会首先检查 `Meta Index Block` 来找到相应的 Meta Block，比如为了确定键是否存在于某个 Data Block 中，它可能需要访问布隆过滤器 Meta Block。

- **优化读取性能**:
   - 通过将这些元数据集中管理，LevelDB 能够优化其读取路径，减少不必要的磁盘访问，从而提高整体性能。


### Footer

`Footer` 是 LevelDB SST 文件的最后一个部分，它包含了一些重要的元数据，用于帮助 LevelDB 快速定位文件中的关键结构，如 `Metaindex Block` 和 `Index Block`。`Footer` 的内容是固定长度的，它位于 SST 文件的末尾。

```
+---------------------------------------------------+
|   Metaindex Block 的 BlockHandle                  |
+---------------------------------------------------+
|   Index Block 的 BlockHandle                      |
+---------------------------------------------------+
|   Padding(填充以达到固定长度)                        |
+---------------------------------------------------+
|   Magic Number(魔数，用于文件格式识别)                |
+---------------------------------------------------+
```

**解释**：

- **Metaindex Block 的 BlockHandle**:
   - 指向 Metaindex Block 的 BlockHandle 包含了 Metaindex Block 在 SST 文件中的偏移量和大小。
   - 通过这个 BlockHandle，LevelDB 可以快速定位到 Metaindex Block。

- **Index Block 的 BlockHandle**:
   - 指向 Index Block 的 BlockHandle 包含了 Index Block 在 SST 文件中的偏移量和大小。
   - 通过这个 BlockHandle，LevelDB 可以快速定位到 Index Block。

- **Padding（填充）**:
   - 为了使 Footer 达到固定长度，可能会添加一些填充字节。

- **Magic Number（魔数）**:
   - 一个特定的数字，用于标识文件的类型和格式。这对于文件格式的正确识别和验证非常重要。