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


