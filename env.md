# leveldb::Env 跨平台运行环境的封装

- [leveldb::Env 跨平台运行环境的封装](#leveldbenv-跨平台运行环境的封装)
  - [Env 接口概览](#env-接口概览)
  - [POSIX 环境下的 Env 的实现](#posix-环境下的-env-的实现)
    - [POSIX 下的 Env::Default() 的实现](#posix-下的-envdefault-的实现)
      - [SingletonEnv 的实现](#singletonenv-的实现)
        - [前置知识: placement new 与 std::aligned\_storage](#前置知识-placement-new-与-stdaligned_storage)
        - [SingletonEnv 的代码实现](#singletonenv-的代码实现)
      - [SingletonEnv 的存在意义](#singletonenv-的存在意义)
    - [PosixEnv 的构造函数](#posixenv-的构造函数)
    - [PosixEnv 的构造函数](#posixenv-的构造函数-1)
    - [PosixEnv::NewSequentialFile(const std::string\& filename, SequentialFile\*\* result)](#posixenvnewsequentialfileconst-stdstring-filename-sequentialfile-result)
      - [SequentialFile](#sequentialfile)
      - [PosixSequentialFile](#posixsequentialfile)
    - [PosixEnv::NewRandomAccessFile(const std::string\& filename, RandomAccessFile\*\* result)](#posixenvnewrandomaccessfileconst-stdstring-filename-randomaccessfile-result)
      - [RandomAccessFile](#randomaccessfile)
      - [PosixRandomAccessFile](#posixrandomaccessfile)
        - [fd\_limiter 是什么](#fd_limiter-是什么)
        - [PosixRandomAccessFile 的实现](#posixrandomaccessfile-的实现)
      - [PosixMmapReadableFile](#posixmmapreadablefile)
      - [PosixMmapReadableFile 与 PosixRandomAccessFile 的区别](#posixmmapreadablefile-与-posixrandomaccessfile-的区别)
    - [PosixEnv::NewWritableFile(const std::string\& fname, WritableFile\*\* result)](#posixenvnewwritablefileconst-stdstring-fname-writablefile-result)
    - [PosixEnv::NewAppendableFile(const std::string\& fname, WritableFile\*\* result)](#posixenvnewappendablefileconst-stdstring-fname-writablefile-result)
    - [PosixEnv::FileExists(const std::string\& filename)](#posixenvfileexistsconst-stdstring-filename)
    - [PosixEnv::GetChildren(const std::string\& directory\_path, std::vectorstd::string\* result)](#posixenvgetchildrenconst-stdstring-directory_path-stdvectorstdstring-result)
    - [PosixEnv::RemoveFile(const std::string\& fname)](#posixenvremovefileconst-stdstring-fname)
    - [PosixEnv::CreateDir(const std::string\& dirname)](#posixenvcreatedirconst-stdstring-dirname)
    - [PosixEnv::RemoveDir(const std::string\& dirname)](#posixenvremovedirconst-stdstring-dirname)
    - [PosixEnv::GetFileSize(const std::string\& fname, uint64\_t\* file\_size)](#posixenvgetfilesizeconst-stdstring-fname-uint64_t-file_size)
    - [PosixEnv::RenameFile(const std::string\& src, const std::string\& target)](#posixenvrenamefileconst-stdstring-src-const-stdstring-target)
    - [PosixEnv::LockFile(const std::string\& fname, FileLock\*\* lock)](#posixenvlockfileconst-stdstring-fname-filelock-lock)
      - [PosixEnv::LockFile 如何保证不同线程之间只有一个线程能获得锁](#posixenvlockfile-如何保证不同线程之间只有一个线程能获得锁)
      - [PosixEnv::LockFile 如何保证不同进程之间只有一个进程能获得锁](#posixenvlockfile-如何保证不同进程之间只有一个进程能获得锁)
    - [PosixEnv::UnlockFile(FileLock\* lock)](#posixenvunlockfilefilelock-lock)
    - [PosixEnv::Schedule(void (\*background\_work\_function)(void\* background\_work\_arg), void\* background\_work\_arg)](#posixenvschedulevoid-background_work_functionvoid-background_work_arg-void-background_work_arg)
      - [PosixEnv::Schedule 的使用姿势](#posixenvschedule-的使用姿势)
      - [PosixEnv::Schedule 的代码实现](#posixenvschedule-的代码实现)
      - [PosixEnv::BackgroundThreadEntryPoint 消费者线程的执行逻辑](#posixenvbackgroundthreadentrypoint-消费者线程的执行逻辑)
    - [PosixEnv::StartThread(void (\*thread\_main)(void\* thread\_main\_arg), void\* thread\_main\_arg)](#posixenvstartthreadvoid-thread_mainvoid-thread_main_arg-void-thread_main_arg)
    - [PosixEnv::GetTestDirectory(std::string\* result)](#posixenvgettestdirectorystdstring-result)
    - [PosixEnv::NewLogger(const std::string\& filename, Logger\*\* result)](#posixenvnewloggerconst-stdstring-filename-logger-result)
      - [PosixLogger](#posixlogger)
        - [Logger 接口](#logger-接口)
        - [PosixLogger 的实现](#posixlogger-的实现)
    - [PosixEnv::NowMicros()](#posixenvnowmicros)
    - [PosixEnv::SleepForMicroseconds()](#posixenvsleepformicroseconds)


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

## POSIX 环境下的 Env 的实现

leveldb 为 POSIX 系统提供了一个 Env 实现，即 PosixEnv。它的实现代码在 `util/env_posix.cc` 文件中。

现在我们来看看 PosixEnv 的实现。

### POSIX 下的 Env::Default() 的实现

`util/env_posix.cc` 文件中定义了 POSIX 环境下`Env::Default()`的实现：

```c++
Env* Env::Default() {
    // 定义一个单例的 PosixDefaultEnv 对象
    static PosixDefaultEnv env_container;
    return env_container.env();
}
```

`util/env_posix.h` 文件中是这么定义 PosixDefaultEnv 的：

```c++
using PosixDefaultEnv = SingletonEnv<PosixEnv>;
```

OK，那我们得先看下`SingletonEnv`的实现。  

#### SingletonEnv 的实现

##### 前置知识: placement new 与 std::aligned_storage

SingletonEnv 的实现主要涉及到两个 C++ 特性：

- placement new
- std::aligned_storage

placement new 是一种原地构造的方式，它可以在已经分配好的内存空间上，原地构造一个对象。

我们一般常用的构造方式有 2 种，一种是在栈上构造，一种是在堆上构造。

```c++
struct MyStruct {};

// 栈上构造
MyStruct my_struct_1();

// 堆上构造
MyStruct* my_struct_2 = nullptr;
my_struct_2 = new MyStruct();
```

栈上构造的好处是内存分配效率高，但是无法延迟构造。比如下面这个场景，就无法使用栈上构造：

```c++
struct Wheel {
    Wheel(int size) : size_(size) {}
    int size_;
};
class Car {
public:
    Car() {
        // wheel 的尺寸需要调用 getSizeOfWheel() 才能知道。
        // 如果 wheels 是一个栈上构造的对象，那么在执行构 Car 的造函数之前，
        // wheels 就会被构造出来了。
        // 然而我们需要在执行 Car 的构造函数的过程中，知道 wheel 尺寸后
        // 再去构造 wheel。
        int size_of_wheel = getSizeOfWheel(); 
        wheel_ = new Wheel(size_of_wheel); 
    }
    ~Car() { delete[] wheel_; }

private:
    Wheel* wheels_;
};
```

而如果我们既想要在栈上构造，又想要延迟构造，就可以和通过 std::aligned_storage 和 placement new 搭配使用达到这一目的。

```c++
struct Wheel {
    Wheel(int size) : size_(size) {}
    int size_;
};
class Car {
public:
    Car() {
        // 先获取 wheel 的尺寸
        int size_of_wheel = getSizeOfWheel(); 
        // 再通过 placement new 在栈上构造 wheel
        new (&hweel_) Wheel(size_of_wheel);
    }
    ~Car() { delete[] wheel_; }

private:
    // 使用 std::aligned_storage 来创建一个足够大并且正确对齐的栈上内存空间 wheel_，
    std::aligned_storage<sizeof(Wheel), alignof(Wheel)>::type wheel_;
};
```

##### SingletonEnv 的代码实现

OK，现在我们可以来看 SingletonEnv 的实现了。

```c++
template <typename EnvType>
class SingletonEnv {
   public:
    SingletonEnv() {
        // NDEBUG 宏表示 NO DEBUG，表示 Release 模式
#if !defined(NDEBUG)
        // 在调试模式下，将 env_initialized_ 标记为 true，表示已经初始化过了。
        // 有些全局变量需要在 SingletonEnv 初始化前就设置好，因为 SingletonEnv
        // 初始化的过程中需要用到这些全局变量，比如 g_open_read_only_file_limit。
        // env_initialized_ 的作用是用来在 UT 中检查是否有在初始化全局变量前
        // 就把 SingletonEnv 初始化了:
        //     // 检查 SingletonEnv 此时是否已经初始化了
        //     PosixDefaultEnv::AssertEnvNotInitialized();
        //     // 设着好 g_open_read_only_file_limit 后再初始化 env
        //     g_open_read_only_file_limit = limit;
        //     env = Env::Default();
        //     // 此时 env 一定是基于指定的 g_open_read_only_file_limit 初始化的
        //     // 此时若 UT 出现错误，就可以排除是 env 提前初始化导致的问题
        env_initialized_.store(true, std::memory_order::memory_order_relaxed);
#endif  // !defined(NDEBUG)
        // static_assert 是在编译期间检查， assert 是在运行期间检查
        static_assert(sizeof(env_storage_) >= sizeof(EnvType), "env_storage_ will not fit the Env");
        static_assert(alignof(decltype(env_storage_)) >= alignof(EnvType),
                      "env_storage_ does not meet the Env's alignment needs");
        // 使用 placement new 的方式，
        // 在 env_storage_ 空间上原地构造一个 EnvType 对象。
        new (&env_storage_) EnvType();
    }
    ~SingletonEnv() = default;

    SingletonEnv(const SingletonEnv&) = delete;
    SingletonEnv& operator=(const SingletonEnv&) = delete;

    //  返回在 env_storage_ 空间上原地构造出来的 Env 对象
    Env* env() { return reinterpret_cast<Env*>(&env_storage_); }

    // 仅供 UT 测试使用
    static void AssertEnvNotInitialized() {
#if !defined(NDEBUG)
        assert(!env_initialized_.load(std::memory_order::memory_order_relaxed));
#endif  // !defined(NDEBUG)
    }

   private:
    // 使用 std::aligned_storage 来创建一个足够大并且正确对齐的内存空间 env_storage_，
    // 用于存放 EnvType 类型的对象。
    // 这里使用 std::aligned_storage 的目的是为了延迟构造 env_storage_。
    // 如果写成 EnvType env_storage_ 的话，那么 env_storage_ 会在 SingletonEnv 的构造函数
    // 执行之前，就进行初始化了。也就是先初始化 env_storage_ 然后
    // 再执行 env_initialized_.store(true, std::memory_order::memory_order_relaxed)。
    // 但此处我们需要先执行 env_initialized_.store(true, std::memory_order::memory_order_relaxed)，
    // 再构造 env_storage_。
    //
    // 个人感觉写成 EnvType* env_storage_ 会不会更简单些？一样可以延迟构造 env_storage_。
    // std::aligned_storage 比 EnvType* 的好处是:
    //   - 栈空间比堆空间的分配效率更高。
    //     - std::aligned_storage 开辟的是一块栈空间，
    //     - EnvType* 使用的是队空间。
    //   - 对齐方式 
    //     - std::aligned_storage 可以强制使用 alignof(EnvType) 的对齐方式
    //     - EnvType* 的对齐方式取决于编译器，新版的编译器都会根据类型自动对齐，老编译器可能不会
    typename std::aligned_storage<sizeof(EnvType), alignof(EnvType)>::type env_storage_;
#if !defined(NDEBUG)
    // env_initialized_ 只用于 UT，Release 模式下不会使用到。
    static std::atomic<bool> env_initialized_;
#endif  // !defined(NDEBUG)
};
```

#### SingletonEnv 的存在意义

在理解 SingletonEnv 的实现后，有的同学可能会有疑问，SingletonEnv 也不负责构造 Env 的单例， 那 SingletonEnv 的存在意义是什么呢？

SingletonEnv 是在 `Env::Default()` 中用到的，我们回头来看下 `Env::Default()` 的实现：

```c++
Env* Env::Default() {
    // 定义一个单例的 PosixDefaultEnv 对象
    static PosixDefaultEnv env_container;
    return env_container.env();
}
```

我们把 `PosixDefaultEnv env_container;` 等价替换为 `static SingletonEnv<PosixEnv> env_container;`:

```c++
Env* Env::Default() {
    static SingletonEnv<PosixEnv> env_container;
    return env_container.env();
}
```

如果把 SingletonEnv 扔掉，就可以写成下面这种最常见的单例模式了：

```c++
Env* Env::Default() {
    static PosixEnv env;
    return &env;
}
```

好像也没什么问题。那 SingletonEnv 的存在意义应该只是封装 Env 的构造过程，方便使用`env_initialized_`进行 UT 测试吧🤣。

知道的大佬请留言赐教一下😅。

### PosixEnv 的构造函数

PosixEnv 的构造函数只是对一些成员变量进行了初始化，这些成员变量分别是:

- background_work_cv_: 用于 PosixEnv 里任务队列的生产者-消费者同步。当任务队列为空时，消费者会调用`background_work_cv_.Wait()`方法休眠直至生产者唤醒自己。
- started_background_thread_: 标记 PosixEnv 里的消费者线程是否已经启动。
- mmap_limit_: 表示可同时进行 mmap 的 region 数量限制。过多的 mmap 可能反而会降低性能。
- fd_limit_: 表示可同时打开的文件数量限制。文件描述符会占用内核资源，过多的文件描述符可能会导致内核资源耗尽。

```c++
PosixEnv::PosixEnv()
    : background_work_cv_(&background_work_mutex_),
      started_background_thread_(false),
      mmap_limiter_(MaxMmaps()),
      fd_limiter_(MaxOpenFiles()) {}
```

mmap 的最大数量限制通过 `MaxMmaps()` 函数获取，它的实现如下：

```c++
// mmap 的默认最大数量限制取决于平台是 64-bit 还是 32-bit:
// - 对于 64-bit 的平台，mmap 的最大数量限制为 1000。
// - 对于 32-bit 的平台，mmap 的最大数量限制为 0。
// 在 32-bit 平台上，LevelDB 将 kDefaultMmapLimit 设置为 0 的原因主要与地址空间的限制有关。
// 在 32-bit 的系统中，整个地址空间（包括用户空间和内核空间）只有 4GB，其中用户空间通常只有 2GB 或 3GB。
// 这意味着可供 mmap 使用的地址空间相对较小。
constexpr const int kDefaultMmapLimit = (sizeof(void*) >= 8) ? 1000 : 0;

// g_mmap_limit 的值为 kDefaultMmapLimit
int g_mmap_limit = kDefaultMmapLimit;

// mmap 的最大数量限制由 g_mmap_limit 决定。
int MaxMmaps() { return g_mmap_limit; }
```

可以得出结论，对于 64bit 的平台，level 使用 mmap 的最大数量限制为 1000；对于 32bit 的平台，leveldb 不允许使用 mmap。

fd 文件描述符的最大数量限制通过 `MaxOpenFiles()` 函数获取，它的实现如下：

```c++
int MaxOpenFiles() {
    if (g_open_read_only_file_limit >= 0) {
        // 如果 g_open_read_only_file_limit 是个有效值，大于等于 0，
        // 则 g_open_read_only_file_limit 就表示最大可同时打开的文件数量。
        return g_open_read_only_file_limit;
    }
    
    // 通过系统调用 ::getrlimit 获取系统的文件描述符最大数量限制。
    struct ::rlimit rlim;
    if (::getrlimit(RLIMIT_NOFILE, &rlim)) {
        // 如果 ::getrlimit 系统调用失败，那么就使用一个固定值 50。
        g_open_read_only_file_limit = 50;
    } else if (rlim.rlim_cur == RLIM_INFINITY) {
        // 如果 ::getrlimit 系统调用返回的是无限制，那么就使用 int 类型的最大值，2^32 - 1。
        g_open_read_only_file_limit = std::numeric_limits<int>::max();
    } else {
        // 如果 ::getrlimit 系统调用返回的是个有限值，那么取该值的 20%。
        g_open_read_only_file_limit = rlim.rlim_cur / 5;
    }
    return g_open_read_only_file_limit;
}
```

### PosixEnv 的构造函数

PosixEnv 的析构函数比较有意思，表示不允许析构 PosixEnv 对象。

```c++
~PosixEnv() override {
    // PosixEnv 是通过 std::aligned_storage 构造的，不会被析构。
    static const char msg[] = "PosixEnv singleton destroyed. Unsupported behavior!\n";
    std::fwrite(msg, 1, sizeof(msg), stderr);
    std::abort();
}
```

PosixEnv 虽然是单例，但单例也会析构呀，如果是 static 的单例，会在`main`函数执行完毕后，或调用`exit`函数时析构 static 对象。

不过 LevelDB 中的 PosixEnv 对象是通过 std::aligned_storage 构造的，不会被析构。

我们来写个 demo 测试一下:

```c++
class CC {
public:
	~CC() {
		static const char msg[] =
			"singleton destroyed.\n";
		std::fwrite(msg, 1, sizeof(msg), stderr);
		std::abort();
	}
};

int main() {

    typename std::aligned_storage<sizeof(CC), alignof(CC)>::type env_storage_;
    CC& cc = *new (&env_storage_) CC();

	return 0;
}
```

在上面的 demo 中，我们通过 std::aligned_storage 构造了一个 CC 对象。如果该 CC 对象被析构了，会输出 "singleton destroyed."。

但是该 demo 运行后，没有输出 "singleton destroyed."，说明 CC 对象没有被析构。

### PosixEnv::NewSequentialFile(const std::string& filename, SequentialFile** result)

```c++
Status NewSequentialFile(const std::string& filename, SequentialFile** result) override {
    // 打开该文件，获取其文件描述符 fd。
    int fd = ::open(filename.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
        *result = nullptr;
        return PosixError(filename, errno);
    }

    // 创建一个 PosixSequentialFile 对象。
    *result = new PosixSequentialFile(filename, fd);
    return Status::OK();
}
```

#### SequentialFile

`SequentialFile`是用于顺序读取文件的接口类，子类需要实现`Read`和`Skip`方法。

```c++
// 定义了一个用于顺序读取文件的接口类
class LEVELDB_EXPORT SequentialFile {
   public:
    SequentialFile() = default;

    // 禁止拷贝
    SequentialFile(const SequentialFile&) = delete;
    SequentialFile& operator=(const SequentialFile&) = delete;

    virtual ~SequentialFile();

    // 尝试从文件中读取最多 n bytes 的数据，放到 scratch 中，
    // 并且将 result 指向 scratch 中的数据。
    // 该方法不保证线程安全。
    virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

    // 跳过文件中的 n bytes 数据。
    // 就是将光标往后移动 n 个字节。
    virtual Status Skip(uint64_t n) = 0;
};
```

#### PosixSequentialFile

我们来看下 PosixSequentialFile 是如何实现 SequentialFile 接口的。

```c++
class PosixSequentialFile final : public SequentialFile {
   public:
    // 由 PosixSequentialFile 接管 fd。
    // 当 PosixSequentialFile 析构时，会负责关闭 fd。
    PosixSequentialFile(std::string filename, int fd) : fd_(fd), filename_(filename) {}
    ~PosixSequentialFile() override { close(fd_); }

    Status Read(size_t n, Slice* result, char* scratch) override {
        Status status;
        while (true) {
            // 尝试通过系统调用 ::read 从文件中读取 n bytes 数据。
            ::ssize_t read_size = ::read(fd_, scratch, n);

            // 如果读取失败，根据失败原因来判断是否需要重试。
            if (read_size < 0) {  // Read error.
                // 碰到因为中断导致的读取失败，就重新读取。
                if (errno == EINTR) {
                    continue;  // Retry
                }
                // 碰到其他原因导致的读取失败，直接返回错误。
                status = PosixError(filename_, errno);
                break;
            }

            // 读取成功，更新 result。
            *result = Slice(scratch, read_size);
            break;
        }
        return status;
    }

    Status Skip(uint64_t n) override {
        // 通过 ::lseek 改变该文件的读写光标。
        if (::lseek(fd_, n, SEEK_CUR) == static_cast<off_t>(-1)) {
            return PosixError(filename_, errno);
        }
        return Status::OK();
    }
};
```

### PosixEnv::NewRandomAccessFile(const std::string& filename, RandomAccessFile** result)

```c++
Status NewRandomAccessFile(const std::string& filename, RandomAccessFile** result) override {
    *result = nullptr;
    // 打开该文件，获取其文件描述符 fd。
    int fd = ::open(filename.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
        return PosixError(filename, errno);
    }

    // 如果 mmap 的数量超出上限了，就跳过 mmap 创建一个 PosixRandomAccessFile 对象。
    if (!mmap_limiter_.Acquire()) {
        *result = new PosixRandomAccessFile(filename, fd, &fd_limiter_);
        return Status::OK();
    }

    // mmap 的数量还没有超出限制，将该文件 mmap 到内存中，然后创建一个 PosixMmapReadableFile 对象。
    uint64_t file_size;
    Status status = GetFileSize(filename, &file_size);
    if (status.ok()) {
        void* mmap_base = ::mmap(/*addr=*/nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
        if (mmap_base != MAP_FAILED) {
            *result = new PosixMmapReadableFile(filename, reinterpret_cast<char*>(mmap_base),
                                                file_size, &mmap_limiter_);
        } else {
            status = PosixError(filename, errno);
        }
    }

    // mmap 已经完成了，可以关闭文件释放 fd 了。
    ::close(fd);
    if (!status.ok()) {
        // 如果 mmap 失败了，需要将 mmap_limiter 修正回来
        mmap_limiter_.Release();
    }
    return status;
}
```

#### RandomAccessFile

`RandomAccessFile`是一个一个用于随机读取文件的接口类，子类需要实现`Read`方法。

```c++
class LEVELDB_EXPORT RandomAccessFile {
   public:
    RandomAccessFile() = default;

    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;

    virtual ~RandomAccessFile();

    // 从文件的 offset 位置开始，尝试读取最多 n bytes 的数据，放到 scratch 中，
    // 并且将 result 指向 scratch 中的数据。
    // 该接口保证线程安全。
    virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};
```

#### PosixRandomAccessFile

`PosixRandomAccessFile`在构造时需要传入`fd`与`fd_limiter`。

`fd`我们都知道是文件描述符，那`fd_limiter`是什么呢？

##### fd_limiter 是什么

`fd_limiter`的类型是`Limiter*`，它是一个计数器，用于限制可同时打开的文件数量。

如果同时打开的文件过多，可能会导致文件描述符耗尽，消耗过多的内核资源。

```c++
class Limiter {
   public:
    // 初始化时传入可用的最大资源数量，将计数器的值初始化为该值。
    Limiter(int max_acquires) : acquires_allowed_(max_acquires) {}

    Limiter(const Limiter&) = delete;
    Limiter operator=(const Limiter&) = delete;

    // 如果当前可用的资源数量大于 0，那么就将计数器减 1，并返回 true。
    // 如果当前可用的资源数量为 0，则返回 true。
    bool Acquire() {
        int old_acquires_allowed = acquires_allowed_.fetch_sub(1, std::memory_order_relaxed);

        if (old_acquires_allowed > 0) return true;

        acquires_allowed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 将计数器的值加 1，表示归还一个资源。
    void Release() { acquires_allowed_.fetch_add(1, std::memory_order_relaxed); }

   private:
    // 计数器
    std::atomic<int> acquires_allowed_;
};
```

##### PosixRandomAccessFile 的实现

`PosixRandomAccessFile`会接管`fd`，析构时负责关闭`fd`。

`PosixRandomAccessFile::Read`通过系统调用`::pread`从指定的`offset`处读取`n` bytes 数据，实现随机读取接口。

```c++
class PosixRandomAccessFile final : public RandomAccessFile {
   public:
    // PosixRandomAccessFile 会接管 fd，析构时负责关闭 fd。
    // fd_limiter 是一个计数器，用于限制一直打开的 fd 的使用数量。
    // fd_limiter->Acquire() 表示从 fd_limiter 中获取一个 fd，
    // 如果使用的 fd 超过限制，fd_limiter->Acquire() 会返回失败。
    // has_permanent_fd_ 的含义是该 fd 是否一直保持打开状态。
    // 如果 has_permanent_fd_ 为 false，每次读前都要打开 fd，读完后再关闭 fd。
    PosixRandomAccessFile(std::string filename, int fd, Limiter* fd_limiter)
        : has_permanent_fd_(fd_limiter->Acquire()),
          fd_(has_permanent_fd_ ? fd : -1),
          fd_limiter_(fd_limiter),
          filename_(std::move(filename)) {
        if (!has_permanent_fd_) {
            assert(fd_ == -1);
            ::close(fd);  // The file will be opened on every read.
        }
    }

    ~PosixRandomAccessFile() override {
        // 如果 fd 是一直保持打开状态的，那么析构时需要关闭 fd，
        // 并且将 fd 归还给 fd_limiter。
        if (has_permanent_fd_) {
            assert(fd_ != -1);
            ::close(fd_);
            fd_limiter_->Release();
        }
    }

    Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
        int fd = fd_;
        // 如果 fd 不是一直保持打开状态的，那么需要先打开 fd。
        if (!has_permanent_fd_) {
            fd = ::open(filename_.c_str(), O_RDONLY | kOpenBaseFlags);
            if (fd < 0) {
                return PosixError(filename_, errno);
            }
        }

        assert(fd != -1);

        Status status;
        // 使用 ::pread 从指定的 offset 处读取 n bytes 数据。
        ssize_t read_size = ::pread(fd, scratch, n, static_cast<off_t>(offset));
        *result = Slice(scratch, (read_size < 0) ? 0 : read_size);
        if (read_size < 0) {
            // An error: return a non-ok status.
            status = PosixError(filename_, errno);
        }

        // 读完后，如果 fd 不需要一直保持打开状态，则关闭 fd。
        if (!has_permanent_fd_) {
            assert(fd != fd_);
            ::close(fd);
        }
        return status;
    }
};
```

#### PosixMmapReadableFile

`PosixMmapReadableFile`是`RandomAccessFile`的另一个实现。与`PosixRandomAccessFile`不同的是，`PosixRandomAccessFile`通过`::pread`从磁盘中读取文件内容，而`PosixMmapReadableFile`使用`mmap`将文件映射到内存中，然后从内存中读取文件内容。

```c++
class PosixMmapReadableFile final : public RandomAccessFile {
   public:
    // 文件的内容都被映射到 mmap_base[0, length-1] 这块内存空间。
    // mmap_limiter 是一个计数器，用于限制 mmap region 的使用数量。
    // 调用者需要先调用 mmap_limiter->Acquire() 获取一个 mmap region 的使用权，
    // PosixMmapReadableFile 在销毁时会调用 mmap_limiter->Release() 归还该 mmap region。
    PosixMmapReadableFile(std::string filename, char* mmap_base, size_t length,
                          Limiter* mmap_limiter)
        : mmap_base_(mmap_base),
          length_(length),
          mmap_limiter_(mmap_limiter),
          filename_(std::move(filename)) {}

    ~PosixMmapReadableFile() override {
        ::munmap(static_cast<void*>(mmap_base_), length_);
        mmap_limiter_->Release();
    }

    Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
        if (offset + n > length_) {
            *result = Slice();
            return PosixError(filename_, EINVAL);
        }

        // 对于已经 mmap 好的文件，直接从内存空间 mmap_base_ 中读取数据。
        *result = Slice(mmap_base_ + offset, n);
        return Status::OK();
    }

   private:
    char* const mmap_base_;
    const size_t length_;
    Limiter* const mmap_limiter_;
    const std::string filename_;
};
```

#### PosixMmapReadableFile 与 PosixRandomAccessFile 的区别

`PosixMmapReadableFile`使用`mmap`将文件映射到内存中，然后从内存中读取文件内容。当我们第一次访问这块 mmap 内存空间时，会触发一次 Page Fault 中断，内核将这部分文件内容从磁盘中读取到内存中。当我们第二次再访问同样的内存空间时，就不需要再进行一次磁盘 IO 了，直接从内存中读取。

`PosixRandomAccessFile`通过`::pread`从磁盘中读取文件内容。每次读取都是从磁盘的文件中读取。

所以对于会反复读取的文件，使用`PosixMmapReadableFile`会比`PosixRandomAccessFile`性能更好。

但是对于只需要读取一次的文件，使用`PosixRandomAccessFile`的开销会更小一些，因为`PosixMmapReadableFile`还需要额外的内存映射管理，建立磁盘上文件内容到进程内存空间的映射关系。

但是在 Linux 平台上，存在 Page Cache 机制，对文件内容进行缓存。当第一次通过`::pread`读取时，内容会被缓存到 Page Cache 中。当第二次再通过`::pread`读取时，就不需要再进行一次磁盘 IO 了，直接从 Page Cache 中读取。

所以在 Linux 平台上，对于反复读取的场景，`PosixMmapReadableFile`和`PosixRandomAccessFile`的性能差异不会太大。

### PosixEnv::NewWritableFile(const std::string& fname, WritableFile** result)

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

PosixWritableFile 的实现可移步参考[TODO: Env 中 WritableFile](TODO)

### PosixEnv::NewAppendableFile(const std::string& fname, WritableFile** result)

```c++
Status NewAppendableFile(const std::string& filename, WritableFile** result) override {
    // 如果文件存在，则在原有文件的尾部追加内容。
    int fd = ::open(filename.c_str(), O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
        *result = nullptr;
        return PosixError(filename, errno);
    }

    // 创建一个 PosixWritableFile 对象
    *result = new PosixWritableFile(filename, fd);
    return Status::OK();
}
```

PosixWritableFile 的实现可移步参考[TODO: Env 中 WritableFile](TODO)

### PosixEnv::FileExists(const std::string& filename)

```c++
bool FileExists(const std::string& filename) override {
    // 甩给系统调用 ::access 判断文件是否存在。
    return ::access(filename.c_str(), F_OK) == 0;
}
```

### PosixEnv::GetChildren(const std::string& directory_path, std::vector<std::string>* result)

`PosixEnv::GetChildren`没什么好说的，通过系统调用`::opendir`和`::readdir`获取目录下的文件名。

```c++
Status GetChildren(const std::string& directory_path,
                    std::vector<std::string>* result) override {
    result->clear();
    // 使用 ::opendir 打开目录，获取目录描述符。
    ::DIR* dir = ::opendir(directory_path.c_str());
    if (dir == nullptr) {
        return PosixError(directory_path, errno);
    }
    struct ::dirent* entry;
    // 通过 ::readdir 迭代获取目录下的文件名。
    while ((entry = ::readdir(dir)) != nullptr) {
        result->emplace_back(entry->d_name);
    }
    ::closedir(dir);
    return Status::OK();
}
```

### PosixEnv::RemoveFile(const std::string& fname)

`PosixEnv::RemoveFile`通过系统调用`::unlink`将该文件从文件系统的目录结构中移除，减少该文件的链接数。

文件的内容不会立即从磁盘上删除。只有当所有打开该文件的文件描述符都被关闭后，文件系统才会释放与文件相关的资源。

```c++
Status RemoveFile(const std::string& filename) override {
    // 一种基于引用计数的删除策略。
    // 使用 ::unlink 将文件从文件系统的目录结构中移除，减少该文件的链接数。
    // 当该文件的链接数降到零，即没有任何文件名指向该文件时，文件系统才会释放该文件占用的空间。
    // 只有当所有打开该文件的文件描述符都被关闭后，文件系统才会释放与文件相关的资源。
    if (::unlink(filename.c_str()) != 0) {
        return PosixError(filename, errno);
    }
    return Status::OK();
}
```

### PosixEnv::CreateDir(const std::string& dirname)

没啥好说，包装了一下系统调用`::mkdir`。

```c++
Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0) {
        return PosixError(dirname, errno);
    }
    return Status::OK();
}
```

### PosixEnv::RemoveDir(const std::string& dirname)

没啥好说，包装了一下系统调用`::rmdir`。

如果目录非空，则删除失败。

```c++
Status RemoveDir(const std::string& dirname) override {
    if (::rmdir(dirname.c_str()) != 0) {
        return PosixError(dirname, errno);
    }
    return Status::OK();
}
```

### PosixEnv::GetFileSize(const std::string& fname, uint64_t* file_size)

通过系统调用`::stat`获取文件信息，里面包含了文件的大小。

```c++
Status GetFileSize(const std::string& filename, uint64_t* size) override {
    struct ::stat file_stat;
    if (::stat(filename.c_str(), &file_stat) != 0) {
        *size = 0;
        return PosixError(filename, errno);
    }
    *size = file_stat.st_size;
    return Status::OK();
}
```

### PosixEnv::RenameFile(const std::string& src, const std::string& target)

封装了一下`std::rename`。

```c++
Status RenameFile(const std::string& from, const std::string& to) override {
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        return PosixError(from, errno);
    }
    return Status::OK();
}
```

### PosixEnv::LockFile(const std::string& fname, FileLock** lock)

`LockFile`的实现比较有意思，它保证了不同线程之间只能有一个线程能成功获得锁，并且不同进程之间，也只能有一个进程能成功获得锁。

```c++
Status LockFile(const std::string& filename, FileLock** lock) override {
    *lock = nullptr;

    // 先获得目标文件的描述符
    int fd = ::open(filename.c_str(), O_RDWR | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
        return PosixError(filename, errno);
    }

    // 在多线程层面，获得该文件的锁
    if (!locks_.Insert(filename)) {
        ::close(fd);
        return Status::IOError("lock " + filename, "already held by process");
    }

    // 在多进程层面，获得该文件的锁
    if (LockOrUnlock(fd, true) == -1) {
        int lock_errno = errno;
        ::close(fd);
        locks_.Remove(filename);
        return PosixError("lock " + filename, lock_errno);
    }

    // 构造一个 PosixFileLock 对象返回。
    *lock = new PosixFileLock(fd, filename);
    return Status::OK();
}
```

#### PosixEnv::LockFile 如何保证不同线程之间只有一个线程能获得锁

先说如何确保不同线程之间只能有一个线程能成功获得锁。

`locks_.Insert(filename)`如果执行成功，该线程就会获得`filename`的锁。其他线程再执行`locks_.Insert(filename)`的时候，就会失败。

`locks_`的类型是`PosixLockTable locks_;`，我们来看下`PosixLockTable`的实现。

```c++
class PosixLockTable {
   public:
    bool Insert(const std::string& fname) LOCKS_EXCLUDED(mu_) {
        mu_.Lock();
        // 往 std::set 中插入重复元素的话，会失败。
        // 利用 std::set 的去重特性，
        // 如果 fname 已经在 locked_files_ 中了，那么就返回 false。
        bool succeeded = locked_files_.insert(fname).second;
        mu_.Unlock();
        return succeeded;
    }
    void Remove(const std::string& fname) LOCKS_EXCLUDED(mu_) {
        mu_.Lock();
        locked_files_.erase(fname);
        mu_.Unlock();
    }

   private:
    port::Mutex mu_;
    std::set<std::string> locked_files_ GUARDED_BY(mu_);
};
```

`PosixLockTable`利用的是`std::set`的去重特性，维护一个`std::set<std::string> locked_files_`，用于存放已经被锁住的文件名。

线程 A 调用`Insert(fname)`将`fname`放入`locked_files_`中后，线程 B 再调用`Insert(fname)`时，由于`fname`已经在`locked_files_`中了，此时线程 B 的`Insert(fname)`会失败。

#### PosixEnv::LockFile 如何保证不同进程之间只有一个进程能获得锁

在多进程层面，是通过`LockOrUnlock(fd, true)`实现进程锁的。

那我们来看下`LockOrUnlock`的实现。

```c++
int LockOrUnlock(int fd, bool lock) {
    // errno 是一个全局变量，用于存储最近一次系统调用的错误号
    errno = 0;

    // 定义一个 flock 结构体
    struct ::flock file_lock_info;
    std::memset(&file_lock_info, 0, sizeof(file_lock_info));

    // 设置加锁|解锁
    file_lock_info.l_type = (lock ? F_WRLCK : F_UNLCK);
    // 设置锁的起始位置为文件的开头
    file_lock_info.l_whence = SEEK_SET;
    // 设置锁的起始位置为 l_whence + 0
    file_lock_info.l_start = 0;
    // 设置锁的长度为 0，表示锁住整个文件
    file_lock_info.l_len = 0;  // Lock/unlock entire file.

    // 调用系统调用 ::fcntl 进行 加锁|解锁 操作
    return ::fcntl(fd, F_SETLK, &file_lock_info);
}
```

`LockOrUnlock`是通过系统调用`::fcntl(F_SETLK)`实现的文件加锁｜上锁。

但是`::fcntl(F_SETLK)`只能在进程的层面上锁，对于同一进程里的多个线程，同时调用`::fcntl(F_SETLK)`，是无法保证只有一个线程能成功获得锁的。


### PosixEnv::UnlockFile(FileLock* lock)

了解`PosixEnv::LockFile`的实现后，`PosixEnv::UnlockFile`的实现就很简单了。

释放进程层面以及线程层面的锁即可。

```c++
Status UnlockFile(FileLock* lock) override {
    // 此处使用 static_cast 而不是 dynamic_cast，
    // 是因为我们已经确定了lock指针的实际类型是 PosixFileLock。
    // static_cast是一种静态转换，它在编译时进行类型检查，并且只能用于已知的类型转换。
    // 它不会进行运行时类型检查，比 dynamic_cast 效率更高。
    PosixFileLock* posix_file_lock = static_cast<PosixFileLock*>(lock);

    // 释放进程层面的锁
    if (LockOrUnlock(posix_file_lock->fd(), false) == -1) {
        return PosixError("unlock " + posix_file_lock->filename(), errno);
    }

    // 释放线程层面的锁
    locks_.Remove(posix_file_lock->filename());
    ::close(posix_file_lock->fd());
    delete posix_file_lock;
    return Status::OK();
}
```

### PosixEnv::Schedule(void (\*background_work_function)(void\* background_work_arg), void\* background_work_arg)

#### PosixEnv::Schedule 的使用姿势

我们先看下`PosixEnv::Schedule`的参数，有两个，一个是`background_work_function`，另一个是`background_work_arg`。

诶？那岂不是限制了`background_work_function`的参数只能有一个`void*`吗？

如果我们有个函数`add`如下，如何传给`PosixEnv::Schedule`呢？

```c++
void add(int num1, int num2, int* sum) {
    *sum = num1 + num2;
}
```

首先，定义一个结构体来保存`add`函数的参数：

```cpp
struct AddArgs {
    int a;
    int b;
    int* c;
};
```

然后，将`add`函数包装一下：

```cpp
void addWrapper(void* arg) {
    AddArgs* args = static_cast<AddArgs*>(arg);
    add(args->a, args->b, args->c);
}
```

此时我们就可以通过`PosixEnv::Schedule`来调用`addWrapper`函数，进而调用`add`了。

```cpp
int result;
AddArgs args = {1, 2, &result};
PosixEnv::Schedule(addWrapper, &args);
```

这样，当`PosixEnv::Schedule`在后台线程中调用`addWrapper`时，`addWrapper`会解包参数并调用`add`函数。

#### PosixEnv::Schedule 的代码实现

理解`PosixEnv::Schedule`的使用姿势后，我们可以来看下它的代码实现了。

简单来说，`PosixEnv::Schedule`就是将`background_work_function`和`background_work_arg`打包成一个任务，然后将该任务放入任务队列`background_work_queue_`中，等待后台的消费者线程来执行。

```c++
void PosixEnv::Schedule(void (*background_work_function)(void* background_work_arg),
                        void* background_work_arg) {
    background_work_mutex_.Lock();

    // 如果后台的消费者线程还没开启，就创建一个消费者线程。
    if (!started_background_thread_) {
        started_background_thread_ = true;
        // 创建一个消费者线程，执行 PosixEnv::BackgroundThreadEntryPoint 方法。
        // PosixEnv::BackgroundThreadEntryPoint 本质上是一个 while 循环，
        // 不停的从任务队列 background_work_queue_ 中取出任务执行。
        std::thread background_thread(PosixEnv::BackgroundThreadEntryPoint, this);
        // 调用 detach 将 background_thread 与当前线程分离，放在后台运行。
        background_thread.detach();
    }

    // 此处可能有点反直觉，在往任务队列中推入任务前，就先把消费者线程唤醒了？
    // 不会的，此时只是先把信号发送出去了，但是 background_work_mutex_ 还没有释放，
    // 消费者线程在拿到 background_work_mutex_ 之前，不会被唤醒。
    if (background_work_queue_.empty()) {
        background_work_cv_.Signal();
    }

    // 将 background_work_function 压入任务队列中，等待消费者线程执行。
    background_work_queue_.emplace(background_work_function, background_work_arg);
    background_work_mutex_.Unlock();
}
```

#### PosixEnv::BackgroundThreadEntryPoint 消费者线程的执行逻辑

我们可以继续看下`PosixEnv::BackgroundThreadEntryPoint`的实现，看下后台的消费者线程是如何从`background_work_queue_`中取出任务并执行的。

```c++
static void BackgroundThreadEntryPoint(PosixEnv* env) { env->BackgroundThreadMain(); }
```

OK，原来`PosixEnv::BackgroundThreadEntryPoint`只是把`PosixEnv::BackgroundThreadMain`包装了一下。

那我们继续看`PosixEnv::BackgroundThreadMain`的实现。

`PosixEnv::BackgroundThreadMain`就是在一个`while`里不停的从任务队列中取出目标任务，并执行。

```c++
void PosixEnv::BackgroundThreadMain() {
    // 不停的从任务队列中取出任务并执行。
    // 如果任务队列为空，那么就调用 background_work_cv_.Wait() 方法休眠，
    // 等待 PosixEnv::Schedule 放入任务后唤醒自己。
    while (true) {
        // 先获得 background_work_mutex_
        background_work_mutex_.Lock();

        // 如果有多个消费者线程，可能会有惊群效应。
        // 有多个线程同时等待并被唤醒，但只有一个线程能够成功地从队列中取出任务。
        // 也有可能会有假唤醒(Spurious Wakeup)的情况，
        // 加个 while 循环可以 cover 这种 case。
        while (background_work_queue_.empty()) {
            background_work_cv_.Wait();
        }

        // 加个 assert，防止 background_work_queue_ 为空时，
        // 还继续往下走，出现不好 debug 的 coredump。
        assert(!background_work_queue_.empty());

        // 从任务队列中取出一个任务，其实就是执行函数和参数。
        auto background_work_function = background_work_queue_.front().function;
        void* background_work_arg = background_work_queue_.front().arg;
        background_work_queue_.pop();

        // 此时任务已经取出来了，可以先释放 background_work_mutex_ 了。
        background_work_mutex_.Unlock();
        // 执行任务函数。
        background_work_function(background_work_arg);
    }
}
```

### PosixEnv::StartThread(void (\*thread_main)(void\* thread_main_arg), void\* thread_main_arg)

`PosixEnv::StartThread`的实现很简单，起一个`std::thread`再`detach`就行。

```c++
void StartThread(void (*thread_main)(void* thread_main_arg), void* thread_main_arg) override {
    std::thread new_thread(thread_main, thread_main_arg);
    new_thread.detach();
}
```


### PosixEnv::GetTestDirectory(std::string* result)

`PosixEnv::GetTestDirectory`的作用是获取一个临时目录，用于 UT 测试。

如果环境变量`TEST_TMPDIR`存在，就使用该环境变量的值。

否则的话，使用`/tmp/leveltest-{有效用户ID}`作为测试目录。

`::geteuid()`是一个Unix系统调用，它返回当前进程的有效用户ID。

在Unix和类Unix系统中，每个进程都有一个实际用户ID和一个有效用户ID。实际用户ID是启动进程的用户的ID，而有效用户ID则决定了进程的权限。

```c++
Status GetTestDirectory(std::string* result) override {
    const char* env = std::getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
        // 如果环境变量 TEST_TMPDIR 存在，就使用该环境变量的值。
        *result = env;
    } else {
        // 否则的话，使用 "/tmp/leveltest-{有效用户ID}" 作为测试目录。
        char buf[100];
        std::snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", static_cast<int>(::geteuid()));
        *result = buf;
    }

    // 创建该测试目录
    CreateDir(*result);

    return Status::OK();
}
```

### PosixEnv::NewLogger(const std::string& filename, Logger** result)

打开目标文件，创建一个`PosixLogger`对象。

```c++
Status NewLogger(const std::string& filename, Logger** result) override {
    // 以追加的方式打开 LOG 文件
    int fd = ::open(filename.c_str(), O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
        *result = nullptr;
        return PosixError(filename, errno);
    }

    // 通过 ::fdopen 将 fd 转换为 FILE*，
    // 然后创建一个 PosixLogger 对象。
    std::FILE* fp = ::fdopen(fd, "w");
    if (fp == nullptr) {
        ::close(fd);
        *result = nullptr;
        return PosixError(filename, errno);
    } else {
        *result = new PosixLogger(fp);
        return Status::OK();
    }
}
```

#### PosixLogger

PosixLogger 是 Logger 接口的实现，我们先看下 Logger 有哪些需要实现的接口。

##### Logger 接口

Logger 接口比较简单，只有一个`Logv`方法，用于将日志信息写入到文件中。

```c++
class LEVELDB_EXPORT Logger {
   public:
    Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    virtual ~Logger();

    // Logger 的子类需要实现该方法，以格式化的形式将日志信息写入到文件中。
    virtual void Logv(const char* format, std::va_list ap) = 0;
};
```

##### PosixLogger 的实现

Logger 接口中只有`Logv`一个方法需要子类来实现。现在我们看下`PosixLogger`是如何实现`Logv`的。

日志格式为: \[时间戳\] \[线程ID\] \[日志内容\]

`PosixLogger::Logv`首先尝试将日志信息写入一个栈上固定大小的缓冲区。如果日志信息太大，无法完全写入栈分配的缓冲区，那么它会使用一个动态分配的缓冲区进行第二次尝试。

然后将缓冲区里的日志内容写入到文件中。

```c++
void Logv(const char* format, std::va_list arguments) override {
    // 打日志时需要添加上时间戳，所以需要先获取当前时间。
    struct ::timeval now_timeval;
    ::gettimeofday(&now_timeval, nullptr);
    const std::time_t now_seconds = now_timeval.tv_sec;
    struct std::tm now_components;
    ::localtime_r(&now_seconds, &now_components);

    // 打日志时需要添加上线程 ID，所以需要先获取当前线程 ID。
    // 通过不同方式获取的线程 ID 可能不同，对于同一个线程来说，
    // 可能 GDB 中看到的线程 ID 是 1234，而 std::this_thread::get_id()
    // 获取到的线程 ID 是 5678。
    // 我们此处获取的线程 ID 不是为了真实要获取它的线程 ID，因为不存在"真实的线程 ID"。
    // 只需要在打 LOG 的时候，我们能够区分出某条日志与其他条日志是否来自同一个线程即可。
    // 所以我们只需要取前 32 位即可，足够区分不同线程了。
    // 同样的做法我们也可以在 git 中看到，git 中每一条 commit 都会有一个 commit ID，
    // git commit ID 的完整长度是 40 个字符，但我们一般取前 7 个字符就足够区分不同的 commit 了。
    constexpr const int kMaxThreadIdSize = 32;
    std::ostringstream thread_stream;
    thread_stream << std::this_thread::get_id();
    std::string thread_id = thread_stream.str();
    if (thread_id.size() > kMaxThreadIdSize) {
        thread_id.resize(kMaxThreadIdSize);
    }

    constexpr const int kStackBufferSize = 512;
    char stack_buffer[kStackBufferSize];
    static_assert(sizeof(stack_buffer) == static_cast<size_t>(kStackBufferSize),
                    "sizeof(char) is expected to be 1 in C++");

    int dynamic_buffer_size = 0;
    for (int iteration = 0; iteration < 2; ++iteration) {
        const int buffer_size = (iteration == 0) ? kStackBufferSize : dynamic_buffer_size;
        char* const buffer = (iteration == 0) ? stack_buffer : new char[dynamic_buffer_size];

        // 把时间戳和线程ID写入 buffer。
        int buffer_offset = std::snprintf(
            buffer, buffer_size, "%04d/%02d/%02d-%02d:%02d:%02d.%06d %s ",
            now_components.tm_year + 1900, now_components.tm_mon + 1, now_components.tm_mday,
            now_components.tm_hour, now_components.tm_min, now_components.tm_sec,
            static_cast<int>(now_timeval.tv_usec), thread_id.c_str());

        assert(buffer_offset <= 28 + kMaxThreadIdSize);
        static_assert(28 + kMaxThreadIdSize < kStackBufferSize,
                        "stack-allocated buffer may not fit the message header");
        assert(buffer_offset < buffer_size);

        // 把日志内容写入 buffer。
        std::va_list arguments_copy;
        va_copy(arguments_copy, arguments);
        // 假设 buffer_size 是 512，写入 时间戳+线程ID 后，buffer_offset 是 40，
        // 那么日志内容写入 buffer 的起始位置是 buffer + 40，最大写入长度是 512 - 40 = 472。
        // 此时如果日志内容的长度超出了 472，比如说日志内容的长度是 500，
        // 那 std::vsnprintf 最多也只会写入日志内容的前 472 个字符，但是
        // 会将实际所需的 buffer 大小返回，也就是 500。
        // buffer_offset 的值就是 40 + 500 = 540。
        // 后面我们就可以通过查看 buffer_offset 的值来判断 buffer 是否足够大。
        // 如果日志内容超出了 buffer 的长度，我们就需要重新分配一个更大的 buffer。
        buffer_offset += std::vsnprintf(buffer + buffer_offset, buffer_size - buffer_offset,
                                        format, arguments_copy);
        va_end(arguments_copy);

        // 把日志内容写入 buffer 后，还需要追加换行符和'\0'结束符，还需要 2 个字符的空间。
        if (buffer_offset >= buffer_size - 1) {
            // 此时 buffer_size - buffer_offset 已经 <= 1 了，
            // 但我们还需要 2 个字符的空间，所以此时 buffer 已经不够用了。

            if (iteration == 0) {
                // 如果这是首轮尝试，我们就将 dynamic_buffer_size 
                // 更新为 buffer_offset + 2，也就是日志内容的长度 + '\n' + '\0'，
                // 下轮 iteration 再在堆上开辟一个 dynamic_buffer_size 的 buffer。
                dynamic_buffer_size = buffer_offset + 2;
                continue;
            }

            // 如果跑到此处，表示我们在第 2 轮 iteration 时，
            // buffer 仍然不够用，这按理是不应该发生的。
            assert(false);
            buffer_offset = buffer_size - 1;
        }

        // 如果日志内容没有以 '\n' 结尾，就手动补一个 '\n'。
        if (buffer[buffer_offset - 1] != '\n') {
            buffer[buffer_offset] = '\n';
            ++buffer_offset;
        }

        // 将 buffer 里的内容写入 fp_，并且对 fp_ 刷盘。
        assert(buffer_offset <= buffer_size);
        std::fwrite(buffer, 1, buffer_offset, fp_);
        std::fflush(fp_);

        // 如果当前是第 2 轮 iteration，buffer 是在堆上分配的，需要手动释放。
        if (iteration != 0) {
            delete[] buffer;
        }
        break;
    }
}
```

### PosixEnv::NowMicros()

通过系统调用`::gettimeofday`获取当前时间，再计算出当前微秒时间戳。

```c++
uint64_t NowMicros() override {
    // 每秒有 1,000,000 微秒
    static constexpr uint64_t kUsecondsPerSecond = 1000000;
    struct ::timeval tv;
    // 获得当前时间
    ::gettimeofday(&tv, nullptr);
    // 当前微秒时间戳 = 秒数 * 1,000,000 + 微秒数
    return static_cast<uint64_t>(tv.tv_sec) * kUsecondsPerSecond + tv.tv_usec;
}
```

### PosixEnv::SleepForMicroseconds()

甩给`std::this_thread::sleep_for`

```c++
void SleepForMicroseconds(int micros) override {
    // 甩给 std::this_thread::sleep_for
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}
```