// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Logger implementation that can be shared by all environments
// where enough posix functionality is available.

#ifndef STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_
#define STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <sys/time.h>
#include <thread>

#include "leveldb/env.h"

namespace leveldb {

// 使用 final 关键字，防止被继承。
// 为什么要防止被继承:
//   - Safety: PosixLogger 的析构中会释放 fp_，如果被继承，
//     子类可能会在析构中使用重复释放 fp_，导致错误。
//   - Optimization: 当一个类被声明为 final 时，
//     编译器可以在编译期间对其进行优化。编译器会将该类里的虚函数
//     转为非虚函数，从而提高性能。
class PosixLogger final : public Logger {
   public:
    // Creates a logger that writes to the given file.
    //
    // The PosixLogger instance takes ownership of the file handle.
    // fp 传给 PosixLogger 后，由 PosixLogger 来接管 fp。
    // 当 PosixLogger 析构时，会负责关闭 fp。
    explicit PosixLogger(std::FILE* fp) : fp_(fp) { assert(fp != nullptr); }

    ~PosixLogger() override { std::fclose(fp_); }

    // 日志格式为: [时间戳] [线程ID] [日志内容]
    void Logv(const char* format, std::va_list arguments) override {
        // Record the time as close to the Logv() call as possible.
        // 打日志时需要添加上时间戳，所以需要先获取当前时间。
        struct ::timeval now_timeval;
        ::gettimeofday(&now_timeval, nullptr);
        const std::time_t now_seconds = now_timeval.tv_sec;
        struct std::tm now_components;
        ::localtime_r(&now_seconds, &now_components);

        // Record the thread ID.
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

        // We first attempt to print into a stack-allocated buffer. If this
        // attempt fails, we make a second attempt with a dynamically allocated
        // buffer.
        constexpr const int kStackBufferSize = 512;
        char stack_buffer[kStackBufferSize];
        static_assert(sizeof(stack_buffer) == static_cast<size_t>(kStackBufferSize),
                      "sizeof(char) is expected to be 1 in C++");

        int dynamic_buffer_size = 0;  // Computed in the first iteration.
        for (int iteration = 0; iteration < 2; ++iteration) {
            const int buffer_size = (iteration == 0) ? kStackBufferSize : dynamic_buffer_size;
            char* const buffer = (iteration == 0) ? stack_buffer : new char[dynamic_buffer_size];

            // Print the header into the buffer.
            // 把时间戳和线程ID写入 buffer。
            int buffer_offset = std::snprintf(
                buffer, buffer_size, "%04d/%02d/%02d-%02d:%02d:%02d.%06d %s ",
                now_components.tm_year + 1900, now_components.tm_mon + 1, now_components.tm_mday,
                now_components.tm_hour, now_components.tm_min, now_components.tm_sec,
                static_cast<int>(now_timeval.tv_usec), thread_id.c_str());

            // The header can be at most 28 characters (10 date + 15 time +
            // 3 delimiters) plus the thread ID, which should fit comfortably
            // into the static buffer.
            assert(buffer_offset <= 28 + kMaxThreadIdSize);
            static_assert(28 + kMaxThreadIdSize < kStackBufferSize,
                          "stack-allocated buffer may not fit the message header");
            assert(buffer_offset < buffer_size);

            // Print the message into the buffer.
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

            // The code below may append a newline at the end of the buffer,
            // which requires an extra character.
            // 把日志内容写入 buffer 后，还需要追加换行符和'\0'结束符，还需要 2 个字符的空间。
            if (buffer_offset >= buffer_size - 1) {
                // 此时 buffer_size - buffer_offset 已经 <= 1 了，
                // 但我们还需要 2 个字符的空间，所以此时 buffer 已经不够用了。

                // The message did not fit into the buffer.
                if (iteration == 0) {
                    // Re-run the loop and use a dynamically-allocated buffer.
                    // The buffer will be large enough for the log message, an
                    // extra newline and a null terminator.
                    // 如果这是首轮尝试，我们就将 dynamic_buffer_size 
                    // 更新为 buffer_offset + 2，也就是日志内容的长度 + '\n' + '\0'，
                    // 下轮 iteration 再在堆上开辟一个 dynamic_buffer_size 的 buffer。
                    dynamic_buffer_size = buffer_offset + 2;
                    continue;
                }

                // The dynamically-allocated buffer was incorrectly sized. This
                // should not happen, assuming a correct implementation of
                // std::(v)snprintf. Fail in tests, recover by truncating the
                // log message in production.
                // 如果跑到此处，表示我们在第 2 轮 iteration 时，
                // buffer 仍然不够用，这按理是不应该发生的。
                assert(false);
                buffer_offset = buffer_size - 1;
            }

            // Add a newline if necessary.
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

   private:
    std::FILE* const fp_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_
