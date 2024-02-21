# Table

- [Table](#table)
  - [Table::Open](#tableopen)
    - [读取 Footer](#读取-footer)
    - [读取 Index Block](#读取-index-block)
    - [创建 Table 对象](#创建-table-对象)
    - [加载 SST 的 MetaData](#加载-sst-的-metadata)
  - [Table::NewIterator](#tablenewiterator)
  - [Table::ApproximateOffsetOf](#tableapproximateoffsetof)
  - [Table::InternalGet](#tableinternalget)
    - [在 Index Block 里查找 Key 对应的 Data BlockHandle](#在-index-block-里查找-key-对应的-data-blockhandle)
    - [在对应的 Data Block 里查找目标 Key](#在对应的-data-block-里查找目标-key)

`Table`就是`SST(Sorted Strings Table)`。

当我们要对一个`SST`进行读取操作时，比如查找一个`Key`，或者遍历这个`SST`，就需要通过`Table`提供的接口来完成。

`Table`将`SST`文件的读取细节封装了起来，让上层不需要去关心`SST`文件的格式和读取细节。

先看下`Table`的接口定义:

```cpp
class LEVELDB_EXPORT Table {
   public:
    // 工厂函数，给定一个打开的 SST 文件，构造一个 Table 对象并返回。
    static Status Open(const Options& options, RandomAccessFile* file, uint64_t file_size, Table** table);

    // 创建一个迭代器，用于遍历 SST 中的键值对。
    Iterator* NewIterator(const ReadOptions&) const;

    // 返回一个 key 在 SST 中的大致偏移量。
    uint64_t ApproximateOffsetOf(const Slice& key) const;

    // 从 SST 中查找某个 Key。如果这个 Key 找到了，则调用 handle_result 函数。
    Status InternalGet(const ReadOptions&, const Slice& key, void* arg,
                       void (*handle_result)(void* arg, const Slice& k, const Slice& v));
};
```

## Table::Open

创建一个`Table`，只需要从`SST`文件中`file`中先把`footer`和``index_block`读出来就可以了。构造好`index_block`，根据`footer`加载`SST`的`MetaData`，一个`Table`对象就准备好了

```cpp
Status Table::Open(const Options& options, RandomAccessFile* file, uint64_t size, Table** table) {
    *table = nullptr;

    // 如果文件大过于小，则判定为文件损坏。
    if (size < Footer::kEncodedLength) {
        return Status::Corruption("file is too short to be an sstable");
    }

    // 1. 先把 footer 读出来。
    char footer_space[Footer::kEncodedLength];
    Slice footer_input;
    Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                          footer_space);
    if (!s.ok()) return s;

    Footer footer;
    s = footer.DecodeFrom(&footer_input);
    if (!s.ok()) return s;

    // 2. 根据 footer 里的 index_handle 读出 index_block_contents。
    BlockContents index_block_contents;
    ReadOptions opt;
    if (options.paranoid_checks) {
        opt.verify_checksums = true;
    }
    s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);

    if (s.ok()) {
        // 3. 由 index_block_contents 构建出 index_block。 
        // index_block 构建出来后，就可以创建出一个 Table 对象了。 
        // 等到需要查找某个 Key 的时候，通过 index_block 找到对应的 
        // BlockHandle，然后再读出对应的 Block。
        Block* index_block = new Block(index_block_contents);
        Rep* rep = new Table::Rep;
        rep->options = options;
        rep->file = file;
        rep->metaindex_handle = footer.metaindex_handle();
        rep->index_block = index_block;
        rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
        rep->filter_data = nullptr;
        rep->filter = nullptr;
        *table = new Table(rep);
        (*table)->ReadMeta(footer);
    }

    return s;
}
```

### 读取 Footer

`Footer`位于`SST`文件的最后，`SST`的格式可移步参考[大白话解析LevelDB：数据格式](https://blog.csdn.net/sinat_38293503/article/details/134739340#SST_93)。

所以通过`file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input, footer_space)`读取`SST`的末尾，即可读出`Footer`的内容。

然后通过`footer.DecodeFrom(&footer_input)`即可创建出对应的`Footer`对象。

### 读取 Index Block

构建好`footer`后，通过`footer.index_handle()`获取到`index_block`的`BlockHandle`，也就是`SST`中`index_block`的偏移量和大小，就可以通过`ReadBlock(file, opt, footer.index_handle(), &index_block_contents)`读取出`index_block`的内容了。

然后就可以通过`Block* index_block = new Block(index_block_contents)`构建出`index_block`了。

`ReadBlock(file, opt, footer.index_handle(), &index_block_contents)`的实现如下:

```cpp
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options, const BlockHandle& handle,
                 BlockContents* result) {
    result->data = Slice();
    result->cachable = false;
    result->heap_allocated = false;

    // handle.offset() 是 Block 在文件中的偏移量，
    // handle.size() 是 Block 的大小，
    // kBlockTrailerSize 是 Block 的尾部信息长度，
    // 包括一个字节的 Block 类型和 4 个字节的 crc 校验和。
    // 根据 handle.offset() 和 handle.size() 可以从 SST 文件中读出 Block 的内容。
    size_t n = static_cast<size_t>(handle.size());
    char* buf = new char[n + kBlockTrailerSize];
    Slice contents;
    Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
    if (!s.ok()) {
        delete[] buf;
        return s;
    }
    // 检查读出来的 block 的长度是否正确。
    if (contents.size() != n + kBlockTrailerSize) {
        delete[] buf;
        return Status::Corruption("truncated block read");
    }

    // 读出 block 的内容后，校验下 CRC 确保数据没有损坏。
    const char* data = contents.data();  // Pointer to where Read put the data
    if (options.verify_checksums) {
        const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
        const uint32_t actual = crc32c::Value(data, n + 1);
        if (actual != crc) {
            delete[] buf;
            s = Status::Corruption("block checksum mismatch");
            return s;
        }
    }

    // 查看 block 内容是否被压缩，如果被压缩了，就解压缩。
    switch (data[n]) {
        case kNoCompression:
            if (data != buf) {
                delete[] buf;
                result->data = Slice(data, n);
                result->heap_allocated = false;
                result->cachable = false;  // Do not double-cache
            } else {
                result->data = Slice(buf, n);
                result->heap_allocated = true;
                result->cachable = true;
            }

            // Ok
            break;
        case kSnappyCompression: {
            size_t ulength = 0;
            if (!port::Snappy_GetUncompressedLength(data, n, &ulength)) {
                delete[] buf;
                return Status::Corruption("corrupted compressed block contents");
            }
            char* ubuf = new char[ulength];
            if (!port::Snappy_Uncompress(data, n, ubuf)) {
                delete[] buf;
                delete[] ubuf;
                return Status::Corruption("corrupted compressed block contents");
            }
            delete[] buf;
            result->data = Slice(ubuf, ulength);
            result->heap_allocated = true;
            result->cachable = true;
            break;
        }
        default:
            delete[] buf;
            return Status::Corruption("bad block type");
    }

    return Status::OK();
}
```

### 创建 Table 对象

构建好`index_block`后，就可以创建出一个`Table`对象了。

```cpp
Rep* rep = new Table::Rep;
rep->options = options;
rep->file = file;
rep->metaindex_handle = footer.metaindex_handle();
rep->index_block = index_block;
rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
rep->filter_data = nullptr;
rep->filter = nullptr;
*table = new Table(rep);
```

创建`Table`前，先创建了一个`Table::Rep`对象，再根据`Table::Rep`对象创建`Table`对象。

这属于`PIMPL`设计模式，将`Table`的数据成员都封装到`Table::Rep`中，让`Table`只暴露出方法接口。`PIMPL`设计模式的好处可移步参考[大白话解析LevelDB：Pimpl模式](https://blog.csdn.net/sinat_38293503/article/details/134830981)。

### 加载 SST 的 MetaData

创建`Table`对象后，还需要先把`SST`的`Meta Block`加载进来。

`Meta Block`在`SST`里的作用可移步参考[大白话解析LevelDB：数据格式](https://blog.csdn.net/sinat_38293503/article/details/134739340#SST_93)。

```cpp
(*table)->ReadMeta(footer);
```

`ReadMeta`的实现如下:

```cpp
void Table::ReadMeta(const Footer& footer) {

    // 目前 LevelDB 里只有一种 Meta Block，就是 Filter Block。 
    // 所以我们就把 Meta Block 直接看成 Filter Block 就行。
    // 如果 options 里没有设置 Filter Policy，那么 Meta Block
    // 也就不需要读取了。
    if (rep_->options.filter_policy == nullptr) {
        return;
    }

    // 只有当 options 里设置了 paranoid_checks 的时候，才会对
    // Meta Block 进行 CRC 校验。
    ReadOptions opt;
    if (rep_->options.paranoid_checks) {
        opt.verify_checksums = true;
    }
    BlockContents contents;
    // 把 Meta Block 的内容读取到 contents 里。
    if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
        return;
    }
    // 根据 Meta Block 的内容构建出 Meta Block 对象。
    Block* meta = new Block(contents);

    // 加载 Filter 到 Table 里。
    Iterator* iter = meta->NewIterator(BytewiseComparator());
    std::string key = "filter.";
    key.append(rep_->options.filter_policy->Name());
    iter->Seek(key);
    if (iter->Valid() && iter->key() == Slice(key)) {
        ReadFilter(iter->value());
    }
    delete iter;
    delete meta;
}
```

`ReadMeta`的核心为`ReadFilter(iter->value())`，`ReadFilter`的实现如下:

```cpp
void Table::ReadFilter(const Slice& filter_handle_value) {
    // 构造出 Filter Block 的 BlockHandle。
    Slice v = filter_handle_value;
    BlockHandle filter_handle;
    if (!filter_handle.DecodeFrom(&v).ok()) {
        return;
    }

    // 根据 options 里的设置，决定是否需要对 Filter Block 进行 CRC 校验。
    ReadOptions opt;
    if (rep_->options.paranoid_checks) {
        opt.verify_checksums = true;
    }
    // 从 SST 文件里读取 Filter Block 的内容。
    BlockContents block;
    if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
        return;
    }
    if (block.heap_allocated) {
        rep_->filter_data = block.data.data();  // Will need to delete later
    }
    // 根据 Filter Block 的内容构造出 Filter 对象。
    // FilterBlockReader 就是 Filter，名字有点误导性。
    rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}
```

## Table::NewIterator

`Table::NewIterator`实际上就是创建一个`TwoLevelIterator`。

`TwoLevelIterator`的实现可移步参考[大白话解析LevelDB: TwoLevelIterator](https://blog.csdn.net/sinat_38293503/article/details/136221944)。

```cpp
Iterator* Table::NewIterator(const ReadOptions& options) const {
    return NewTwoLevelIterator(rep_->index_block->NewIterator(rep_->options.comparator),
                               &Table::BlockReader, const_cast<Table*>(this), options);
}
```

## Table::ApproximateOffsetOf

`Table::ApproximateOffsetOf`用于返回一个`key`在`SST`中的大致偏移量。

如果该`key`在`SST`中，那么返回该`key`所在的`Data Block`在`SST`中的偏移量。

如果该`key`不在`SST`中，那么返回一个接近`SST`末尾的偏移量。

```cpp
uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
    // 创建一个 index_block 的迭代器。
    Iterator* index_iter = rep_->index_block->NewIterator(rep_->options.comparator);
    // 通过 index_iter 先找到 key 对应的 Data BlockHandle。
    // Data BlockHandle 里包含了 Data Block 的大小和在 SST 文件中的偏移量。
    index_iter->Seek(key);
    uint64_t result;
    if (index_iter->Valid()) {
        // 找到 key 对应的 Data BlockHandle 了。
        BlockHandle handle;
        Slice input = index_iter->value();
        Status s = handle.DecodeFrom(&input);
        if (s.ok()) {
            // 将 Key 所在的 Data Block 在 SST 文件
            // 中的偏移量作为结果返回。
            result = handle.offset();
        } else {
            // 找到了对应 Data BlockHandle，但是解析失败。
            // 那就按没找到 Data BlockHandle 来处理，返回 
            // Meta Block 在 SST 文件中的偏移量，也就是接近
            // SST 文件末尾的一个位置。
            result = rep_->metaindex_handle.offset();
        }
    } else {
        // 没有找到对应的 Data BlockHandle，说明 key 不在
        // 该 SST 里，那就返回 Meta Block 在 SST 文件中的偏移量，
        // 也就是一个接近 SST 文件末尾的位置。
        result = rep_->metaindex_handle.offset();
    }
    delete index_iter;
    return result;
}
```

## Table::InternalGet

`Table::InternalGet`用于从`SST`中查找某个`Key`。如果这个`Key`找到了，则调用`handle_result`函数。

```cpp
Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg,
                          void (*handle_result)(void*, const Slice&, const Slice&)) {
    Status s;

    // 创建一个 index_block 的 iterator，通过这个 iterator
    // 找到 key 对应的 Data BlockHandle。
    Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
    iiter->Seek(k);
    if (iiter->Valid()) {
        // 找到对应的 Data BlockHandle了，先通过 filter 判断
        // 一下目标 key 是否在对应的 Data Block 里。
        Slice handle_value = iiter->value();
        FilterBlockReader* filter = rep_->filter;
        BlockHandle handle;
        if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() &&
            !filter->KeyMayMatch(handle.offset(), k)) {
            // 目标 Key 不在对应的 Data Block 里
        } else {
            // 目标 Key 在对应的 Data Block 里，根据
            // Data BlockHandle 找到 Data Block 的
            // 位置和大小，然后读取出 Data Block。
            Iterator* block_iter = BlockReader(this, options, iiter->value());
            // 到 Data Block 中查找目标 Key。
            block_iter->Seek(k);
            // 如果在 Data Block 中查找到目标 Key 了，
            // 就取出 Value，然后 callback handle_resutl。
            if (block_iter->Valid()) {
                (*handle_result)(arg, block_iter->key(), block_iter->value());
            }
            s = block_iter->status();
            delete block_iter;
        }
    }
    if (s.ok()) {
        s = iiter->status();
    }
    delete iiter;
    return s;
}
```

### 在 Index Block 里查找 Key 对应的 Data BlockHandle

对`Index Block`不了解的同学可以先喵一眼[大白话解析LevelDB：数据格式](https://blog.csdn.net/sinat_38293503/article/details/134739340#SST_93)。

先创建一个`Index Block`的迭代器`iiter`，然后通过`iiter->Seek(k)`找到`key`对应的`Data BlockHandle`。

```cpp
Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
iiter->Seek(k);
```

`rep_->index_block->NewIterator(rep_->options.comparator)`的实现如下，属于工厂模式，将`Block::Iter`的创建细节封装了起来，对使用者透明(使用者不需要了解`Block::Iter`的实现细节)。

```cpp
Iterator* Block::NewIterator(const Comparator* comparator) {
    if (size_ < sizeof(uint32_t)) {
        return NewErrorIterator(Status::Corruption("bad block contents"));
    }
    const uint32_t num_restarts = NumRestarts();
    if (num_restarts == 0) {
        return NewEmptyIterator();
    } else {
        return new Iter(comparator, data_, restart_offset_, num_restarts);
    }
}
```

对`Block::Iter`的实现感兴趣的同学可以移步参考[大白话解析LevelDB: Block Iterator](https://blog.csdn.net/sinat_38293503/article/details/136221870)。

### 在对应的 Data Block 里查找目标 Key

如果`iiter->Seek(k)`找到了`key`对应的`Data BlockHandle`， `Data BlockHandle`里存储了`Data Block`在`SST`文件中的偏移量和大小。

接下来就可以通过`BlockReader`依据`Data BlockHandle`里的信息读取出`Data Block`，然后再在`Data Block`里查找目标`Key`。

但是在读取`Data Block`之前，还需要先通过`Filter`过滤一下，看看目标`Key`是否在`Data Block`里。这样可以减少无效的`IO`开销。

如果没有`Filter`，目标`Key`不在`Data Block`里，我们需要读取了对应的`Data Block`，才发现目标`Key`不在里面，这样就浪费了一次`IO`，读取一次`Data Block`的`IO`开销是比较大的。

```cpp
if (iiter->Valid()) {
    // 找到对应的 Data BlockHandle了，先通过 filter 判断
    // 一下目标 key 是否在对应的 Data Block 里。
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
        // 目标 Key 不在对应的 Data Block 里
    } else {
        // 目标 Key 在对应的 Data Block 里，根据
        // Data BlockHandle 找到 Data Block 的
        // 位置和大小，然后读取出 Data Block。
        Iterator* block_iter = BlockReader(this, options, iiter->value());
        // 到 Data Block 中查找目标 Key。
        block_iter->Seek(k);
        // 如果在 Data Block 中查找到目标 Key 了，
        // 就取出 Value，然后 callback handle_resutl。
        if (block_iter->Valid()) {
            (*handle_result)(arg, block_iter->key(), block_iter->value());
        }
        s = block_iter->status();
        delete block_iter;
    }
}
```
