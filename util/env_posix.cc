// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <queue>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/env_posix_test_helper.h"
#include "util/posix_logger.h"

namespace leveldb {

namespace {

// Set by EnvPosixTestHelper::SetReadOnlyMMapLimit() and MaxOpenFiles().
int g_open_read_only_file_limit = -1;

// Up to 1000 mmap regions for 64-bit binaries; none for 32-bit.
// mmap 的默认最大数量限制取决于平台是 64-bit 还是 32-bit:
// - 对于 64-bit 的平台，mmap 的最大数量限制为 1000。
// - 对于 32-bit 的平台，mmap 的最大数量限制为 0。
// 在 32-bit 平台上，LevelDB 将 kDefaultMmapLimit 设置为 0 的原因主要与地址空间的限制有关。
// 在 32-bit 的系统中，整个地址空间（包括用户空间和内核空间）只有 4GB，其中用户空间通常只有 2GB 或 3GB。
// 这意味着可供 mmap 使用的地址空间相对较小。
constexpr const int kDefaultMmapLimit = (sizeof(void*) >= 8) ? 1000 : 0;

// Can be set using EnvPosixTestHelper::SetReadOnlyMMapLimit().
int g_mmap_limit = kDefaultMmapLimit;

// Common flags defined for all posix open operations
#if defined(HAVE_O_CLOEXEC)
constexpr const int kOpenBaseFlags = O_CLOEXEC;
#else
constexpr const int kOpenBaseFlags = 0;
#endif  // defined(HAVE_O_CLOEXEC)

constexpr const size_t kWritableFileBufferSize = 65536;

Status PosixError(const std::string& context, int error_number) {
    if (error_number == ENOENT) {
        return Status::NotFound(context, std::strerror(error_number));
    } else {
        return Status::IOError(context, std::strerror(error_number));
    }
}

// Helper class to limit resource usage to avoid exhaustion.
// Currently used to limit read-only file descriptors and mmap file usage
// so that we do not run out of file descriptors or virtual memory, or run into
// kernel performance problems for very large databases.
class Limiter {
   public:
    // Limit maximum number of resources to |max_acquires|.
    Limiter(int max_acquires) : acquires_allowed_(max_acquires) {}

    Limiter(const Limiter&) = delete;
    Limiter operator=(const Limiter&) = delete;

    // If another resource is available, acquire it and return true.
    // Else return false.
    bool Acquire() {
        int old_acquires_allowed = acquires_allowed_.fetch_sub(1, std::memory_order_relaxed);

        if (old_acquires_allowed > 0) return true;

        acquires_allowed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Release a resource acquired by a previous call to Acquire() that returned
    // true.
    void Release() { acquires_allowed_.fetch_add(1, std::memory_order_relaxed); }

   private:
    // The number of available resources.
    //
    // This is a counter and is not tied to the invariants of any other class,
    // so it can be operated on safely using std::memory_order_relaxed.
    std::atomic<int> acquires_allowed_;
};

// Implements sequential read access in a file using read().
//
// Instances of this class are thread-friendly but not thread-safe, as required
// by the SequentialFile API.
class PosixSequentialFile final : public SequentialFile {
   public:
    PosixSequentialFile(std::string filename, int fd) : fd_(fd), filename_(filename) {}
    ~PosixSequentialFile() override { close(fd_); }

    Status Read(size_t n, Slice* result, char* scratch) override {
        Status status;
        while (true) {
            ::ssize_t read_size = ::read(fd_, scratch, n);
            if (read_size < 0) {  // Read error.
                if (errno == EINTR) {
                    continue;  // Retry
                }
                status = PosixError(filename_, errno);
                break;
            }
            *result = Slice(scratch, read_size);
            break;
        }
        return status;
    }

    Status Skip(uint64_t n) override {
        if (::lseek(fd_, n, SEEK_CUR) == static_cast<off_t>(-1)) {
            return PosixError(filename_, errno);
        }
        return Status::OK();
    }

   private:
    const int fd_;
    const std::string filename_;
};

// Implements random read access in a file using pread().
//
// Instances of this class are thread-safe, as required by the RandomAccessFile
// API. Instances are immutable and Read() only calls thread-safe library
// functions.
class PosixRandomAccessFile final : public RandomAccessFile {
   public:
    // The new instance takes ownership of |fd|. |fd_limiter| must outlive this
    // instance, and will be used to determine if .
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
        if (has_permanent_fd_) {
            assert(fd_ != -1);
            ::close(fd_);
            fd_limiter_->Release();
        }
    }

    Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
        int fd = fd_;
        if (!has_permanent_fd_) {
            fd = ::open(filename_.c_str(), O_RDONLY | kOpenBaseFlags);
            if (fd < 0) {
                return PosixError(filename_, errno);
            }
        }

        assert(fd != -1);

        Status status;
        ssize_t read_size = ::pread(fd, scratch, n, static_cast<off_t>(offset));
        *result = Slice(scratch, (read_size < 0) ? 0 : read_size);
        if (read_size < 0) {
            // An error: return a non-ok status.
            status = PosixError(filename_, errno);
        }
        if (!has_permanent_fd_) {
            // Close the temporary file descriptor opened earlier.
            assert(fd != fd_);
            ::close(fd);
        }
        return status;
    }

   private:
    const bool has_permanent_fd_;  // If false, the file is opened on every read.
    const int fd_;                 // -1 if has_permanent_fd_ is false.
    Limiter* const fd_limiter_;
    const std::string filename_;
};

// Implements random read access in a file using mmap().
//
// Instances of this class are thread-safe, as required by the RandomAccessFile
// API. Instances are immutable and Read() only calls thread-safe library
// functions.
class PosixMmapReadableFile final : public RandomAccessFile {
   public:
    // mmap_base[0, length-1] points to the memory-mapped contents of the file.
    // It must be the result of a successful call to mmap(). This instances
    // takes over the ownership of the region.
    //
    // |mmap_limiter| must outlive this instance. The caller must have already
    // aquired the right to use one mmap region, which will be released when
    // this instance is destroyed.
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

        *result = Slice(mmap_base_ + offset, n);
        return Status::OK();
    }

   private:
    char* const mmap_base_;
    const size_t length_;
    Limiter* const mmap_limiter_;
    const std::string filename_;
};

class PosixWritableFile final : public WritableFile {
   public:
    PosixWritableFile(std::string filename, int fd)
        : pos_(0),
          fd_(fd),
          is_manifest_(IsManifest(filename)),
          filename_(std::move(filename)),
          dirname_(Dirname(filename_)) {}

    ~PosixWritableFile() override {
        if (fd_ >= 0) {
            // Ignoring any potential errors
            Close();
        }
    }

    // 首先尝试将数据拷贝到缓冲区，如果缓冲区被打满了，就将缓冲区的数据 flush 到文件。
    // 然后，对于剩余的数据:
    //   - 如果能被缓冲区装下，那么将数据拷贝到缓冲区
    //   - 否则，直接将数据写入到文件
    Status Append(const Slice& data) override {
        size_t write_size = data.size();
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

    Status Close() override {
        // 关闭前先把缓冲区里的数据 flush 到文件
        Status status = FlushBuffer();
        // 通过系统调用 ::close 关闭文件
        const int close_result = ::close(fd_);
        if (close_result < 0 && status.ok()) {
            status = PosixError(filename_, errno);
        }
        fd_ = -1;
        return status;
    }

    Status Flush() override { return FlushBuffer(); }

    Status Sync() override {
        // Ensure new files referred to by the manifest are in the filesystem.
        //
        // This needs to happen before the manifest file is flushed to disk, to
        // avoid crashing in a state where the manifest refers to files that are
        // not yet on disk.
        // 将 manifest 文件所在的目录刷盘。
        // 如果当前 WritableFile 是个 manifest 文件，那么在将该 manifest 刷盘前，
        // 需要先将该 manifest 文件所在的目录刷盘，确保其所在目录已经先创建出来了，
        // 然后再刷盘该 manifest 文件。
        Status status = SyncDirIfManifest();
        if (!status.ok()) {
            return status;
        }

        // 将缓冲区里的数据 flush 到文件(其实是内核缓冲区中)
        status = FlushBuffer();
        if (!status.ok()) {
            return status;
        }

        // call 系统调用 ::fsync 将内核缓冲区中的数据刷盘
        return SyncFd(fd_, filename_);
    }

   private:
    Status FlushBuffer() {
        // 将缓冲区里的数据写入到文件
        Status status = WriteUnbuffered(buf_, pos_);
        // 清空缓冲区
        pos_ = 0;
        return status;
    }

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

    Status SyncDirIfManifest() {
        Status status;
        // 如果不是 manifest 文件的话，直接返回 OK
        if (!is_manifest_) {
            return status;
        }

        // 打开 manifest 文件所在的目录，获取其文件描述符
        int fd = ::open(dirname_.c_str(), O_RDONLY | kOpenBaseFlags);
        if (fd < 0) {
            status = PosixError(dirname_, errno);
        } else {
            // 将该目录刷盘
            status = SyncFd(fd, dirname_);
            ::close(fd);
        }
        return status;
    }

    // Ensures that all the caches associated with the given file descriptor's
    // data are flushed all the way to durable media, and can withstand power
    // failures.
    //
    // The path argument is only used to populate the description string in the
    // returned Status if an error occurs.
    static Status SyncFd(int fd, const std::string& fd_path) {
#if HAVE_FULLFSYNC
        // On macOS and iOS, fsync() doesn't guarantee durability past power
        // failures. fcntl(F_FULLFSYNC) is required for that purpose. Some
        // filesystems don't support fcntl(F_FULLFSYNC), and require a fallback
        // to fsync().
        // 在 macOS 和 iOS 平台上，仅仅只是使用 fsync() 并不能保证数据在掉电后的持久化，
        // 需要配合 fcntl(F_FULLFSYNC)。
        if (::fcntl(fd, F_FULLFSYNC) == 0) {
            return Status::OK();
        }
#endif  // HAVE_FULLFSYNC

        // 如果该平台支持 fdatasync 的话，就用 fdatasync 刷盘，
        // 否则的话就用 fsync 刷盘。
        // fdatasync 与 fsync 的区别在于，fdatasync 只会刷盘文件的 data 部分，
        // 而 fsync 会刷盘文件的 data 部分和 meta 部分。meta 部分包含一些文件信息，
        // 如文件大小，文件更新时间等。
        // fdatasync 比 fsync 更高效。
#if HAVE_FDATASYNC
        bool sync_success = ::fdatasync(fd) == 0;
#else
        bool sync_success = ::fsync(fd) == 0;
#endif  // HAVE_FDATASYNC

        if (sync_success) {
            return Status::OK();
        }
        return PosixError(fd_path, errno);
    }

    // Returns the directory name in a path pointing to a file.
    //
    // Returns "." if the path does not contain any directory separator.
    static std::string Dirname(const std::string& filename) {
        std::string::size_type separator_pos = filename.rfind('/');
        if (separator_pos == std::string::npos) {
            return std::string(".");
        }
        // The filename component should not contain a path separator. If it
        // does, the splitting was done incorrectly.
        assert(filename.find('/', separator_pos + 1) == std::string::npos);

        return filename.substr(0, separator_pos);
    }

    // Extracts the file name from a path pointing to a file.
    //
    // The returned Slice points to |filename|'s data buffer, so it is only
    // valid while |filename| is alive and unchanged.
    static Slice Basename(const std::string& filename) {
        std::string::size_type separator_pos = filename.rfind('/');
        if (separator_pos == std::string::npos) {
            return Slice(filename);
        }
        // The filename component should not contain a path separator. If it
        // does, the splitting was done incorrectly.
        assert(filename.find('/', separator_pos + 1) == std::string::npos);

        return Slice(filename.data() + separator_pos + 1, filename.length() - separator_pos - 1);
    }

    // True if the given file is a manifest file.
    static bool IsManifest(const std::string& filename) {
        return Basename(filename).starts_with("MANIFEST");
    }

    // buf_[0, pos_ - 1] contains data to be written to fd_.
    char buf_[kWritableFileBufferSize];
    size_t pos_;
    int fd_;

    const bool is_manifest_;  // True if the file's name starts with MANIFEST.
    const std::string filename_;
    const std::string dirname_;  // The directory of filename_.
};

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

// Instances are thread-safe because they are immutable.
class PosixFileLock : public FileLock {
   public:
    PosixFileLock(int fd, std::string filename) : fd_(fd), filename_(std::move(filename)) {}

    int fd() const { return fd_; }
    const std::string& filename() const { return filename_; }

   private:
    const int fd_;
    const std::string filename_;
};

// Tracks the files locked by PosixEnv::LockFile().
//
// We maintain a separate set instead of relying on fcntl(F_SETLK) because
// fcntl(F_SETLK) does not provide any protection against multiple uses from the
// same process.
//
// Instances are thread-safe because all member data is guarded by a mutex.
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

class PosixEnv : public Env {
   public:
    PosixEnv();
    ~PosixEnv() override {
        // PosixEnv 是通过 std::aligned_storage 构造的，不会被析构。
        static const char msg[] = "PosixEnv singleton destroyed. Unsupported behavior!\n";
        std::fwrite(msg, 1, sizeof(msg), stderr);
        std::abort();
    }

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

    bool FileExists(const std::string& filename) override {
        return ::access(filename.c_str(), F_OK) == 0;
    }

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

    Status RemoveFile(const std::string& filename) override {
        // 一种基于引用计数的删除策略。
        // 使用 ::unlink 将文件名从文件系统的目录结构中移除，减少该文件的链接数。
        // 当该文件的链接数降到零，即没有任何文件名指向该文件时，文件系统才会释放该文件占用的空间。
        // 只有当所有打开该文件的文件描述符都被关闭后，文件系统才会释放与文件相关的资源。
        if (::unlink(filename.c_str()) != 0) {
            return PosixError(filename, errno);
        }
        return Status::OK();
    }

    Status CreateDir(const std::string& dirname) override {
        if (::mkdir(dirname.c_str(), 0755) != 0) {
            return PosixError(dirname, errno);
        }
        return Status::OK();
    }

    Status RemoveDir(const std::string& dirname) override {
        if (::rmdir(dirname.c_str()) != 0) {
            return PosixError(dirname, errno);
        }
        return Status::OK();
    }

    Status GetFileSize(const std::string& filename, uint64_t* size) override {
        struct ::stat file_stat;
        if (::stat(filename.c_str(), &file_stat) != 0) {
            *size = 0;
            return PosixError(filename, errno);
        }
        *size = file_stat.st_size;
        return Status::OK();
    }

    Status RenameFile(const std::string& from, const std::string& to) override {
        if (std::rename(from.c_str(), to.c_str()) != 0) {
            return PosixError(from, errno);
        }
        return Status::OK();
    }

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

    void Schedule(void (*background_work_function)(void* background_work_arg),
                  void* background_work_arg) override;

    void StartThread(void (*thread_main)(void* thread_main_arg), void* thread_main_arg) override {
        std::thread new_thread(thread_main, thread_main_arg);
        new_thread.detach();
    }

    Status GetTestDirectory(std::string* result) override {
        const char* env = std::getenv("TEST_TMPDIR");
        if (env && env[0] != '\0') {
            *result = env;
        } else {
            char buf[100];
            std::snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", static_cast<int>(::geteuid()));
            *result = buf;
        }

        // The CreateDir status is ignored because the directory may already
        // exist.
        CreateDir(*result);

        return Status::OK();
    }

    Status NewLogger(const std::string& filename, Logger** result) override {
        int fd = ::open(filename.c_str(), O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
        if (fd < 0) {
            *result = nullptr;
            return PosixError(filename, errno);
        }

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

    uint64_t NowMicros() override {
        static constexpr uint64_t kUsecondsPerSecond = 1000000;
        struct ::timeval tv;
        ::gettimeofday(&tv, nullptr);
        return static_cast<uint64_t>(tv.tv_sec) * kUsecondsPerSecond + tv.tv_usec;
    }

    void SleepForMicroseconds(int micros) override {
        std::this_thread::sleep_for(std::chrono::microseconds(micros));
    }

   private:
    void BackgroundThreadMain();

    static void BackgroundThreadEntryPoint(PosixEnv* env) { env->BackgroundThreadMain(); }

    // Stores the work item data in a Schedule() call.
    //
    // Instances are constructed on the thread calling Schedule() and used on
    // the background thread.
    //
    // This structure is thread-safe beacuse it is immutable.
    struct BackgroundWorkItem {
        explicit BackgroundWorkItem(void (*function)(void* arg), void* arg)
            : function(function), arg(arg) {}

        void (*const function)(void*);
        void* const arg;
    };

    port::Mutex background_work_mutex_;
    port::CondVar background_work_cv_ GUARDED_BY(background_work_mutex_);
    bool started_background_thread_ GUARDED_BY(background_work_mutex_);

    std::queue<BackgroundWorkItem> background_work_queue_ GUARDED_BY(background_work_mutex_);

    PosixLockTable locks_;  // Thread-safe.
    Limiter mmap_limiter_;  // Thread-safe.
    Limiter fd_limiter_;    // Thread-safe.
};

// Return the maximum number of concurrent mmaps.
int MaxMmaps() { return g_mmap_limit; }

// Return the maximum number of read-only files to keep open.
int MaxOpenFiles() {
    if (g_open_read_only_file_limit >= 0) {
        // 如果 g_open_read_only_file_limit 是个有效值，大于等于 0，
        // 则 g_open_read_only_file_limit 就表示最大可同时打开的文件数量。
        return g_open_read_only_file_limit;
    }
    
    // 通过系统调用 ::getrlimit 获取系统的文件描述符最大数量限制。
    struct ::rlimit rlim;
    if (::getrlimit(RLIMIT_NOFILE, &rlim)) {
        // getrlimit failed, fallback to hard-coded default.
        // 如果 ::getrlimit 系统调用失败，那么就使用一个固定值 50。
        g_open_read_only_file_limit = 50;
    } else if (rlim.rlim_cur == RLIM_INFINITY) {
        // 如果 ::getrlimit 系统调用返回的是无限制，那么就使用 int 类型的最大值，2^32 - 1。
        g_open_read_only_file_limit = std::numeric_limits<int>::max();
    } else {
        // Allow use of 20% of available file descriptors for read-only files.
        // 如果 ::getrlimit 系统调用返回的是个有限值，那么取该值的 20%。
        g_open_read_only_file_limit = rlim.rlim_cur / 5;
    }
    return g_open_read_only_file_limit;
}

}  // namespace

// background_work_cv_ 用于任务队列的生产者-消费者同步。当任务队列为空时，
// 消费者会调用 background_work_cv_.Wait() 方法休眠直至生产者唤醒自己。
// started_background_thread_ 表示消费者线程是否已经启动。
// mmap_limit_ 表示可同时进行 mmap 的 region 数量限制。
// fd_limit_ 表示可同时打开的文件数量限制。
PosixEnv::PosixEnv()
    : background_work_cv_(&background_work_mutex_),
      started_background_thread_(false),
      mmap_limiter_(MaxMmaps()),
      fd_limiter_(MaxOpenFiles()) {}

/* Schedule 方法比较简单，只是将对应的任务函数和函数参数推入至工作队列中，由
 * BackgroundThreadMain() 方法取出任务并执行 */
void PosixEnv::Schedule(void (*background_work_function)(void* background_work_arg),
                        void* background_work_arg) {
    background_work_mutex_.Lock();

    // Start the background thread, if we haven't done so already.
    if (!started_background_thread_) {
        started_background_thread_ = true;
        /* 启动 Entry Point
         * 线程，本质上就是一个死循环，从任务队列中取出任务并执行 */
        std::thread background_thread(PosixEnv::BackgroundThreadEntryPoint, this);
        /* 线程要么调用 detach() 和当前线程分离，要么调用 join()
         * 方法等待其执行完毕 */
        background_thread.detach();
    }

    // If the queue is empty, the background thread may be waiting for work.
    if (background_work_queue_.empty()) {
        background_work_cv_.Signal();
    }

    background_work_queue_.emplace(background_work_function, background_work_arg);
    background_work_mutex_.Unlock();
}

/* 从 background_work_queue_
 * 中取出一个任务并执行，由于任务队列并没有最大数量限制，所以不需要
 * 在取出数据以后 notify 其它线程，一个很简易的实现 */
void PosixEnv::BackgroundThreadMain() {
    while (true) {
        background_work_mutex_.Lock();

        // Wait until there is work to be done.
        while (background_work_queue_.empty()) {
            background_work_cv_.Wait();
        }

        assert(!background_work_queue_.empty());
        auto background_work_function = background_work_queue_.front().function;
        void* background_work_arg = background_work_queue_.front().arg;
        background_work_queue_.pop();

        background_work_mutex_.Unlock();
        background_work_function(background_work_arg);
    }
}

namespace {

// Wraps an Env instance whose destructor is never created.
//
// Intended usage:
//   using PlatformSingletonEnv = SingletonEnv<PlatformEnv>;
//   void ConfigurePosixEnv(int param) {
//     PlatformSingletonEnv::AssertEnvNotInitialized();
//     // set global configuration flags.
//   }
//   Env* Env::Default() {
//     static PlatformSingletonEnv default_env;
//     return default_env.env();
//   }
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

#if !defined(NDEBUG)
template <typename EnvType>
std::atomic<bool> SingletonEnv<EnvType>::env_initialized_;
#endif  // !defined(NDEBUG)

using PosixDefaultEnv = SingletonEnv<PosixEnv>;

}  // namespace

void EnvPosixTestHelper::SetReadOnlyFDLimit(int limit) {
    PosixDefaultEnv::AssertEnvNotInitialized();
    g_open_read_only_file_limit = limit;
}

void EnvPosixTestHelper::SetReadOnlyMMapLimit(int limit) {
    PosixDefaultEnv::AssertEnvNotInitialized();
    g_mmap_limit = limit;
}

Env* Env::Default() {
    // 定义一个单例的 PosixDefaultEnv 对象
    static PosixDefaultEnv env_container;
    return env_container.env();
}

}  // namespace leveldb
