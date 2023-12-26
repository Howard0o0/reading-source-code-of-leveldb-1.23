# leveldb::Env 跨平台运行环境的封装

Env 类在 LevelDB 中是一个抽象基类，它定义了一组虚拟方法，这些方法封装了所有与操作系统环境交互的操作。这包括文件操作（如打开、读取、写入、关闭文件），线程创建和同步操作（如互斥锁和条件变量），以及获取系统相关信息（如当前时间，或者某个文件的大小）等。

这种设计使得 LevelDB 可以在不同的操作系统和平台上运行，只需要提供一个特定平台的 Env 实现。例如，LevelDB 提供了一个针对 POSIX 系统的 Env 实现。

## Env 接口概览

```c++
class LEVELDB_EXPORT Env {
   public:
    Env();

    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;

    virtual ~Env();

    // 返回适用于当前操作系统的 Env 单例。
    static Env* Default();

    // 指定一个文件名，创建一个对应的 SequentialFile 对象，用于顺序读取该文件。
    // example usage: 
    //     SequentialFile* file;
    //     std::string filename = "file-to-sequential-read";  
    //     Status s = env->NewSequentialFile(filename, &file);
    //     // 读取文件内容
    //     const size_t kBufferSize = 1024;
    //     char buffer[kBufferSize];
    //     Slice result;
    //     // 从 file 里读取 kBufferSize 个字节到 result 里
    //     s = file->Read(kBufferSize, &result, buffer);
    //     // 从上次读取的末尾位置开始，再从 file 里读取 kBufferSize 个字节到 result 里
    //     s = file->Read(kBufferSize, &result, buffer);
    virtual Status NewSequentialFile(const std::string& fname, SequentialFile** result) = 0;

    // 指定一个文件名，创建一个对应的 RandomAccessFile 对象，用于随机读取该文件。
    virtual Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) = 0;

    // 指定一个文件名，创建一个对应的 WritableFile 对象，用于将数据写入到该文件。
    // NewWritableFile 与 NewAppendableFile 唯一不一样的地方在于:
    //   - NewWritableFile: 如果文件存在，会先删除该文件，然后再创建一个新的文件。
    //   - NewAppendableFile: 如果文件存在，会直接在该文件后面追加数据。
    virtual Status NewWritableFile(const std::string& fname, WritableFile** result) = 0;

    // 指定一个文件名，创建一个对应的 WritableFile 对象，用于将数据写入到该文件。
    // NewWritableFile 与 NewAppendableFile 唯一不一样的地方在于:
    //   - NewWritableFile: 如果文件存在，会先删除该文件，然后再创建一个新的文件。
    //   - NewAppendableFile: 如果文件存在，会直接在该文件后面追加数据。
    virtual Status NewAppendableFile(const std::string& fname, WritableFile** result);

    // 判断指定文件是否存在
    virtual bool FileExists(const std::string& fname) = 0;

    // 获取指定目录 dir 下的所有一级文件名(不包括子目录里)，放到 result 里
    virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;

    // 删除指定文件
    virtual Status RemoveFile(const std::string& fname);

    // 删除一个目录，如果该目录非空，则会删除失败
    virtual Status RemoveDir(const std::string& dirname);

    // 获取指定文件的大小
    virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) = 0;

    // 重命名文件
    virtual Status RenameFile(const std::string& src, const std::string& target) = 0;

    // 给指定文件加锁，用于给数据库上锁，防止多个进程同时打开同一个数据库。
    // leveldb 会在数据库所在目录下创建一个文件 "LOCK"，打开数据库前需要先
    // 尝试获得 "LOCK" 文件的锁，如果获得锁成功，则表示没有其他进程在访问该
    // 数据库，此时可以打开数据库；否则，表示有其他进程在访问该数据库，打开数据库
    // 失败。
    virtual Status LockFile(const std::string& fname, FileLock** lock) = 0;

    // 解锁数据库，与 LockFile 搭配食用。
    // 用于关闭数据库时释放数据库的文件锁。
    virtual Status UnlockFile(FileLock* lock) = 0;

    // 将指定的函数 function(arg) 放到后台线程池中，线程池中有空闲线程的时候
    // 会执行该函数。
    virtual void Schedule(void (*function)(void* arg), void* arg) = 0;

    // 启动一个新线程来运行指定函数。
    // 当该函数结束时，线程会被销毁。
    virtual void StartThread(void (*function)(void* arg), void* arg) = 0;

    // 获取一个临时目录，用于 UT 测试。
    virtual Status GetTestDirectory(std::string* path) = 0;

    // 根据指定文件名，创建一个 LOG 文件。
    virtual Status NewLogger(const std::string& fname, Logger** result) = 0;

    // 获取微秒时间戳。
    virtual uint64_t NowMicros() = 0;

    // 让当前线程休眠 n 微秒。
    virtual void SleepForMicroseconds(int micros) = 0;
};
```