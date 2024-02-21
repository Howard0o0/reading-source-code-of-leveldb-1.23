# TableCache

- [TableCache](#tablecache)
  - [TableCache 的构造函数](#tablecache-的构造函数)
  - [TableCache::Get](#tablecacheget)
    - [在 Cache 中查找指定的 SST](#在-cache-中查找指定的-sst)
    - [从 SST 中查找指定的 Key](#从-sst-中查找指定的-key)
  - [TableCache::NewIterator](#tablecachenewiterator)
  - [TableCache::Evict](#tablecacheevict)

`TableCache`在`LevelDB`中的作用是管理和缓存`SST`(Sorted String Tables)的读取。

为了提高读取效率，`TableCache`会缓存已打开的`SST`。这样，对同一`SST`的多次读取操作就不需要每次都打开文件。

我们来看下`TableCache`里都有哪些接口:

```cpp
class TableCache {
   public:
    // 构造时接受一个 entries 参数，用于指定最大的缓存 SST 数量。当缓存的 SST 数量超过
    // 这个限制时，TableCache 会根据某种策略（如最近最少使用，LRU）从 Cache 里移除一些
    / SST。
    TableCache(const std::string& dbname, const Options& options, int entries);
    ~TableCache();

    // 返回一个指定 SST 的迭代器，用于遍历 SST 中的键值对。
    Iterator* NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                          Table** tableptr = nullptr);

    // 从指定 SST 中查找某个 Key。如果这个 Key 找到了，则调用 handle_result 函数。
    Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size, const Slice& k,
               void* arg, void (*handle_result)(void*, const Slice&, const Slice&));

    // 将某个 SST 从 TableCache 中移除。
    void Evict(uint64_t file_number);
};
```

## TableCache 的构造函数

```cpp
TableCache::TableCache(const std::string& dbname, const Options& options, int entries)
    : env_(options.env), dbname_(dbname), options_(options), cache_(NewLRUCache(entries)) {}
```

`TableCache`的构造函数里主要是`cache_`的初始化，构造一个`LRUCache`。

`TableCache`其实是一个包装类，核心是`cache_`。`TableCache`的所有接口都是对`cache_`的封装，方便使用。

`NewLRUCache(entries)`是个典型的工厂模式，用于创建一个`LRUCache`对象。`LRUCache`的实现可移步参考[TODO](TODO)。

```cpp
Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }
```

使用工厂模式的好处是替换方便，如果我们想要替换成其他类型的`LRUCache`，比如`SingleLRUCache`，只需要修改`NewLRUCache`函数即可，而不需要每一处构造`LRUCache`的上层代码。

## TableCache::Get

`TableCache::Get`用于从`Cache`中查找指定的`SST`，再从这个`SST`中查找指定的`Key`。

如果`SST`不在`Cache`中，`TableCache`会打开这个`SST`，并将其添加到`Cache`中。

```cpp
Status TableCache::Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                       const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&, const Slice&)) {
    
    // 在 Cache 中找到指定的 SST。
    // 如果目标 SST 不在缓存中，它会打开文件并将其添加到 Cache。
    // handle 指向 Cache 中的 SST Item。
    Cache::Handle* handle = nullptr;
    Status s = FindTable(file_number, file_size, &handle);
    if (s.ok()) {
        // 通过 handle 在 cache 中获取 SST 对应的 Table 对象。
        Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
        // 调用 Table::InternalGet() 方法从 SST 中查找指定的 key。
        s = t->InternalGet(options, k, arg, handle_result);
        cache_->Release(handle);
    }
    return s;
}
```

### 在 Cache 中查找指定的 SST

`TableCache::Get`的核心是`FindTable`函数，它用于在`Cache`中查找指定的`SST`。

先尝试在`cache_`中查找指定的`SST`，如果找到了，就直接返回`handle`。

如果没找到，就打开这个`SST`，并将其添加到`cache_`中，然后再返`handle`。

```cpp
Status TableCache::FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle) {
    Status s;

    // 将 file_number 编码为 fixed64，作为 key 到
    // cache_ 中查找 handle。
    char buf[sizeof(file_number)];
    EncodeFixed64(buf, file_number);
    Slice key(buf, sizeof(buf));
    *handle = cache_->Lookup(key);

    // 如果 cache_ 中木有找到，就打开该 SST 文件，并将其添加到 cache_ 中。
    if (*handle == nullptr) {
        // 根据 file_number 构造出 SST 的文件名。
        // 早期版本的 LevelDB 使用的是 .sst 后缀，后来改为了 .ldb。
        // 为了兼容这两种命名方式，这里会尝试两种后缀。
        // TableFileName() 会构建 .ldb 后缀的 SST 文件名，
        // SSTTableFileName() 会构建 .sst 后缀的 SST 文件名。
        std::string fname = TableFileName(dbname_, file_number);
        RandomAccessFile* file = nullptr;
        Table* table = nullptr;
        s = env_->NewRandomAccessFile(fname, &file);
        if (!s.ok()) {
            std::string old_fname = SSTTableFileName(dbname_, file_number);
            if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
                s = Status::OK();
            }
        }

        // SST 文件打开后，通过 Table::Open 创建一个 Table 对象。
        if (s.ok()) {
            s = Table::Open(options_, file, file_size, &table);
        }

        if (!s.ok()) {
            // 如果创建 Table 对象失败，就关闭 SST 文件的句柄。
            assert(table == nullptr);
            delete file;
        } else {
            // Table 对象创建成功，将其添加到 cache_ 中。
            TableAndFile* tf = new TableAndFile;
            tf->file = file;
            tf->table = table;
            *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
        }
    }
    return s;
}
```
`TableCache::FindTable`里的核心操作是`cache_->Lookup`与`cache_->Insert`。

其实现细节可移步参考[TODO]()与[TODO]()。

`env_->NewRandomAccessFile(fname, &file)`的实现细节可移步参考[大白话解析LevelDB: Env](https://blog.csdn.net/sinat_38293503/article/details/135310073#PosixEnvNewRandomAccessFileconst_stdstring_filename_RandomAccessFile_result_552)。

`Table::Open(options_, file, file_size, &table)`的实现细节可移步参考[大白话解析LevelDB: Table](https://blog.csdn.net/sinat_38293503/article/details/136222084?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22136222084%22%2C%22source%22%3A%22sinat_38293503%22%7D#TableOpen_29)。

### 从 SST 中查找指定的 Key

找到`SST`后就好说了，从`SST`中查找指定的`Key`的逻辑甩给`Table::InternalGet`函数就行了。

`t->InternalGet(options, k, arg, handle_result)`的实现细节可移步参考[大白话解析LevelDB: Table](https://blog.csdn.net/sinat_38293503/article/details/136222084?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22136222084%22%2C%22source%22%3A%22sinat_38293503%22%7D#TableInternalGet_336)。

## TableCache::NewIterator

`TableCache::NewIterator`与`TableCache::Get`类似，先在`Cache`中查找指定的`SST`，再把`NewIterator`的逻辑甩给`Table::NewIterator`函数。

```cpp
Iterator* TableCache::NewIterator(const ReadOptions& options, uint64_t file_number,
                                  uint64_t file_size, Table** tableptr) {
    if (tableptr != nullptr) {
        *tableptr = nullptr;
    }

    // 在 Cache 中找到指定的 SST。
    // 如果目标 SST 不在缓存中，它会打开文件并将其添加到 Cache。
    // handle 指向 Cache 中的 SST Item。
    Cache::Handle* handle = nullptr;
    Status s = FindTable(file_number, file_size, &handle);
    if (!s.ok()) {
        return NewErrorIterator(s);
    }

    // 通过 handle 在 cache 中获取 SST 对应的 Table 对象。
    Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    // 调用 Table::NewIterator() 方法创建该 SST 的 Iterator。
    Iterator* result = table->NewIterator(options);
    result->RegisterCleanup(&UnrefEntry, cache_, handle);
    if (tableptr != nullptr) {
        *tableptr = table;
    }
    return result;
}
```

`table->NewIterator(options)`的实现细节可参考[大白话解析LevelDB: Table](https://blog.csdn.net/sinat_38293503/article/details/136222084?csdn_share_tail=%7B%22type%22%3A%22blog%22%2C%22rType%22%3A%22article%22%2C%22rId%22%3A%22136222084%22%2C%22source%22%3A%22sinat_38293503%22%7D#TableNewIterator_280)。


## TableCache::Evict

将`file_number`包装成一个`cache_`能识别的`Key`，再调用`cache_->Erase`函数，将这个`Key`从`cache_`中移除。

```cpp
void TableCache::Evict(uint64_t file_number) {
    // 将 file_number 编码为 fixed64，
    // 作为 cache_ 中的 key，将该 key 从
    // cache_ 中移除。
    char buf[sizeof(file_number)];
    EncodeFixed64(buf, file_number);
    cache_->Erase(Slice(buf, sizeof(buf)));
}
```

`cache_->Erase(Slice(buf, sizeof(buf)))`的实现细节可移步参考[TODO]()。

