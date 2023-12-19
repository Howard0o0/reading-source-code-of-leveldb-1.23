# WritableFile

由于文件写入在不同平台(比如posix && win)需要使用不同的接口，所以LevelDB将文件写入相关的操作抽象出了一个接口`WritableFile`，如下：

```c++
class LEVELDB_EXPORT WritableFile {
   public:
    WritableFile() = default;

    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;

    virtual ~WritableFile();

    // 往文件中追加数据
    virtual Status Append(const Slice& data) = 0;
    // 关闭文件
    virtual Status Close() = 0;
    // 将缓冲区里的数据 flush 到文件，并清空缓冲区
    virtual Status Flush() = 0;
    // 将内核缓冲区里的数据刷盘
    virtual Status Sync() = 0;
};
```

`WritableFile`的实现必须是带有缓冲机制的，因为调用者可能会一次只写入一小部分数据。

如果不带缓冲机制，每次写入少量数据时都要调用一次系统调用 `write`，会降低写入性能很差(频繁的系统调用会增加开销)。

LevelDB 中提供了两种实现`WritableFile`的方式：

- `PosixWritableFile`：基于`posix`的文件写入实现
- `WinWritableFile`：基于`win`的文件写入实现

本文只关注`PosixWritableFile`的实现，嘿嘿。

## WritableFile 的构造

`WritableFile`的构造由`Env::NewWritableFile(const std::string& fname, WritableFile** result)`来负责。

在 posix 环境下，`Env::NewWritableFile`的实现如下：

```c++
Status NewWritableFile(const std::string& filename, WritableFile** result) override {
    // O_TRUNC: 如果文件已存在，则将其清空。
    // O_WRONLY: 以只写方式打开文件。
    // O_CREAT: 如果文件不存在，则创建文件。
    // kOpenBaseFlags: 一些基本的 flags，比如 O_CLOEXEC。
    int fd = ::open(filename.c_str(), O_TRUNC | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
        *result = nullptr;
        return PosixError(filename, errno);
    }

    // 创建一个 PosixWritableFile 对象
    *result = new PosixWritableFile(filename, fd);
    return Status::OK();
}
```

`O_CLOEXEC`的作用可移步参考[这里](https://blog.csdn.net/yueguangmuyu/article/details/118603354)。

通过阅读`Env::NewWritableFile`的代码实现，我们知道了`WritableFile`在 posix 环境下是通过`new`一个`PosixWritableFile`对象来实现的。

那么接下来我们看下`PosixWritableFile`这个类的实现。  

## PosixWritableFile

### PosixWritableFile 的构造

`PosixWritableFile`的构造很简单，只是将`filename`与`fd`传入，保存到`PosixWritableFile`的成员变量中即可。

这里顺便介绍下`PosixWritableFile`里各个成员变量:

- `pos_`：当前文件的写入位置
- `fd_`：文件描述符
- `is_manifest_`：是否是 manifest 文件。如果是 manifest 文件，那么在写入时会加锁。
- `filename_`：文件名
- `dirname_`：文件所在目录名。传入的`filename`是一个绝对路径，可以从`filename`中获取`dirname_`。

```c++
PosixWritableFile(std::string filename, int fd)
    : pos_(0),
        fd_(fd),
        is_manifest_(IsManifest(filename)),
        filename_(std::move(filename)),
        dirname_(Dirname(filename_)) {}
```


### PosixWritableFile::Append(const Slice& data)

Append方法的作用是将数据追加到文件。

首先尝试将数据尽可能多的拷贝到 PosixWritableFile 的缓冲区里，如果缓冲区被打满了，就将缓冲区的数据 flush 到文件，然后清空缓冲区。
然后，对于剩余的数据:
  - 如果能被缓冲区装下，那么将数据拷贝到缓冲区
  - 否则，直接将数据写入到文件

```c++
Status Append(const Slice& data) override {
    // 待写入的数据大小
    size_t write_size = data.size();
    // 待写入的数据
    const char* write_data = data.data();

    // Fit as much as possible into buffer.
    // 计算可以拷贝到缓冲区的数据大小，取write_size和缓冲区剩余空间的较小值
    size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);
    // 把能拷贝的数据拷贝到缓冲区
    std::memcpy(buf_ + pos_, write_data, copy_size);
    // 更新 write_data: 指向待写入的数据
    write_data += copy_size;
    // 更新 write_size: 待写入的数据大小
    write_size -= copy_size;
    // 更新 pos_: 缓冲区中可写入数据的位置
    pos_ += copy_size;

    // 如果把数据拷贝到缓冲区，待写入到数据大小为 0 了，表示要写入到数据
    // 已经全部放到缓冲区里了，此时可直接返回，等下次再写入数据把缓冲区打
    // 满了再把缓冲区里的数据 flush 到文件。
    if (write_size == 0) {
        return Status::OK();
    }

    // Can't fit in buffer, so need to do at least one write.
    // 缓冲区的剩余空间无法一次性装下待写入的数据，此时需要通过 FlushBuffer 方法
    // 先将缓冲区的数据 flush 到文件，并清空缓冲区。
    Status status = FlushBuffer();
    if (!status.ok()) {
        return status;
    }

    // Small writes go to buffer, large writes are written directly.
    // 缓冲区里的数据清空后，此时的待写入数据若可以被缓冲区装下，
    // 那么就将数据拷贝到缓冲区，然后返回。
    if (write_size < kWritableFileBufferSize) {
        std::memcpy(buf_, write_data, write_size);
        pos_ = write_size;
        return Status::OK();
    }

    // 待写入数据还是无法被缓冲区装下，那将这部分的待写入数据直接写入文件。
    return WriteUnbuffered(write_data, write_size);
}
```

### PosixWritableFile::FlushBuffer()

`FlushBuffer`方法的作用是将缓冲区的数据 flush 到文件，并清空缓冲区。

```c++
Status FlushBuffer() {
    // 将缓冲区里的数据写入到文件
    Status status = WriteUnbuffered(buf_, pos_);
    // 清空缓冲区
    pos_ = 0;
    return status;
}
```

### PosixWritableFile::WriteUnbuffered(const char* data, size_t size)

`WriteUnbuffered`方法的作用是将数据直接写入到文件。它会循环调用write系统调用，直到所有数据都已写入。

```c++
Status WriteUnbuffered(const char* data, size_t size) {
    // 只要待写入数据大小还大于 0，就一直尝试写入
    while (size > 0) {
        // 通过系统调用 ::write 将数据写入到文件
        ssize_t write_result = ::write(fd_, data, size);
        // write_result < 0，表示 ::write 系统调用失败。
        if (write_result < 0) {
            // 如果只是因为中断导致的写入失败，那么尝试重新写入
            if (errno == EINTR) {
                continue;  // Retry
            }
            // 如果是其他原因导致的写入失败，那么返回错误
            return PosixError(filename_, errno);
        }
        // write_result > 0，表示成功写入到文件的数据大小。
        // 有可能我们通过系统调用 ::write 写入 10KB 的数据，但只会成功写入一部分，
        // 比如当磁盘空间不足的时候就会这样。
        // 
        // 更新 data 与 size，继续尝试写入剩余的数据。
        data += write_result;
        size -= write_result;
    }
    return Status::OK();
}
```