// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"

#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"

namespace leveldb {

const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
struct DBImpl::Writer {
    explicit Writer(port::Mutex* mu) : batch(nullptr), sync(false), done(false), cv(mu) {}

    Status status;
    WriteBatch* batch;
    bool sync;
    bool done;
    port::CondVar cv;
};

struct DBImpl::CompactionState {
    // Files produced by compaction
    struct Output {
        uint64_t number;
        uint64_t file_size;
        InternalKey smallest, largest;
    };

    Output* current_output() { return &outputs[outputs.size() - 1]; }

    explicit CompactionState(Compaction* c)
        : compaction(c), smallest_snapshot(0), outfile(nullptr), builder(nullptr), total_bytes(0) {}

    Compaction* const compaction;

    // Sequence numbers < smallest_snapshot are not significant since we
    // will never have to service a snapshot below smallest_snapshot.
    // Therefore if we have seen a sequence number S <= smallest_snapshot,
    // we can drop all entries for the same key with sequence numbers < S.
    SequenceNumber smallest_snapshot;

    std::vector<Output> outputs;

    // State kept for output being generated
    WritableFile* outfile;
    TableBuilder* builder;

    uint64_t total_bytes;
};

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
    if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
    if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname, const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy, const Options& src) {
    Options result = src;
    result.comparator = icmp;
    result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
    ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
    ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
    ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
    ClipToRange(&result.block_size, 1 << 10, 4 << 20);
    if (result.info_log == nullptr) {
        // Open a log file in the same directory as the db
        src.env->CreateDir(dbname);  // In case it does not exist
        src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
        Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
        if (!s.ok()) {
            // No place suitable for logging
            result.info_log = nullptr;
        }
    }
    if (result.block_cache == nullptr) {
        result.block_cache = NewLRUCache(8 << 20);
    }
    return result;
}

static int TableCacheSize(const Options& sanitized_options) {
    // Reserve ten files or so for other uses and give the rest to TableCache.
    return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      internal_filter_policy_(raw_options.filter_policy),
      options_(
          SanitizeOptions(dbname, &internal_comparator_, &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
      db_lock_(nullptr),
      shutting_down_(false),
      background_work_finished_signal_(&mutex_),
      mem_(nullptr),
      imm_(nullptr),
      has_imm_(false),
      logfile_(nullptr),
      logfile_number_(0),
      log_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_compaction_scheduled_(false),
      manual_compaction_(nullptr),
      versions_(new VersionSet(dbname_, &options_, table_cache_, &internal_comparator_)) {}

DBImpl::~DBImpl() {
    // Wait for background work to finish.
    mutex_.Lock();
    shutting_down_.store(true, std::memory_order_release);
    while (background_compaction_scheduled_) {
        background_work_finished_signal_.Wait();
    }
    mutex_.Unlock();

    if (db_lock_ != nullptr) {
        env_->UnlockFile(db_lock_);
    }

    delete versions_;
    if (mem_ != nullptr) mem_->Unref();
    if (imm_ != nullptr) imm_->Unref();
    delete tmp_batch_;
    delete log_;
    delete logfile_;
    delete table_cache_;

    if (owns_info_log_) {
        delete options_.info_log;
    }
    if (owns_cache_) {
        delete options_.block_cache;
    }
}

Status DBImpl::NewDB() {
    VersionEdit new_db;
    new_db.SetComparatorName(user_comparator()->Name());
    new_db.SetLogNumber(0);
    new_db.SetNextFile(2);
    new_db.SetLastSequence(0);

    const std::string manifest = DescriptorFileName(dbname_, 1);
    WritableFile* file;
    Status s = env_->NewWritableFile(manifest, &file);
    if (!s.ok()) {
        return s;
    }
    {
        log::Writer log(file);
        std::string record;
        new_db.EncodeTo(&record);
        s = log.AddRecord(record);
        if (s.ok()) {
            s = file->Sync();
        }
        if (s.ok()) {
            s = file->Close();
        }
    }
    delete file;
    if (s.ok()) {
        // Make "CURRENT" file that points to the new manifest file.
        s = SetCurrentFile(env_, dbname_, 1);
    } else {
        env_->RemoveFile(manifest);
    }
    return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
    if (s->ok() || options_.paranoid_checks) {
        // No change needed
    } else {
        Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
        *s = Status::OK();
    }
}

// 这个方法的主要作用是进行垃圾收集，删除不再需要的文件，以释放磁盘空间。
// 随着时间的推移，可能会生成大量的临时文件或者过时的版本文件，
// 这些文件如果不及时删除，可能会占用大量的磁盘空间。
void DBImpl::RemoveObsoleteFiles() {
    mutex_.AssertHeld();

    if (!bg_error_.ok()) {
        // After a background error, we don't know whether a new version may
        // or may not have been committed, so we cannot safely garbage collect.
        return;
    }

    // Make a set of all of the live files
    // 将所有需要用到的 SST 文件编号都记录到 live 中。
    //   - pending_outputs_: 正在进行 compaction 的 SST
    //   - versions_->AddLiveFiles(&live): 所有 version 里的 SST 
    std::set<uint64_t> live = pending_outputs_;
    versions_->AddLiveFiles(&live);

    // 获取 leveldb 目录下的所有文件名
    std::vector<std::string> filenames;
    env_->GetChildren(dbname_, &filenames);  // Ignoring errors on purpose
    uint64_t number;
    FileType type;

    // 遍历 leveldb 目录下的所有文件，
    // 把不再需要的文件记录到 files_to_delete 中。
    std::vector<std::string> files_to_delete;
    for (std::string& filename : filenames) {
        // 对于每个文件名，都调用 ParseFileName 解析出文件编号和文件类型。
        if (ParseFileName(filename, &number, &type)) {
            bool keep = true;
            switch (type) {
                case kLogFile:
                    // number >= versions_->LogNumber()，
                    // 表示这个 WAL 是最新或者未来可能需要的 WAL，
                    // 需要保留。
                    // number == versions_->PrevLogNumber()，
                    // 表示这个日志文件是上一个日志文件，
                    // 可能包含了一些还未被合并到 SST 文件的数据，也需要保留。
                    keep = ((number >= versions_->LogNumber()) ||
                            (number == versions_->PrevLogNumber()));
                    break;
                case kDescriptorFile:
                    // Keep my manifest file, and any newer incarnations'
                    // (in case there is a race that allows other incarnations)
                    // number >= versions_->ManifestFileNumber()，
                    // 表示这个 MANIFEST 文件是最新或者未来可能需要的 MANIFEST 文件，
                    // 需要保留。
                    keep = (number >= versions_->ManifestFileNumber());
                    break;
                case kTableFile:
                    // 之前已经将所需要的 SST 文件编号都记录到 live 中了，
                    // 如果当前 SST 文件编号在 live 中不存在，就表示不再需要了。
                    keep = (live.find(number) != live.end());
                    break;
                case kTempFile:
                    // Any temp files that are currently being written to must
                    // be recorded in pending_outputs_, which is inserted into
                    // "live"
                    // 临时文件指正在进行 compaction 的 SST 文件，之前也已经提前
                    // 记录到 live 中了。如果当前临时文件不存在 live 中，表示不再需要了。
                    keep = (live.find(number) != live.end());
                    break;
                case kCurrentFile:
                    // CURRENT 文件，需要一直保留。
                case kDBLockFile:
                    // 文件锁，用来防止多个进程同时打开同一个数据库的，需要一直保留。
                case kInfoLogFile:
                    // INFO 日志文件，需要一直保留。
                    keep = true;
                    break;
            }

            if (!keep) {
                // 如果当前文件不需要保留了，将它加入到 files_to_delete 中，
                // 后面再一起删除。
                files_to_delete.push_back(std::move(filename));
                // 如果被删除的文件是个 SST，还需要把它从 table_cache_ 中移除。
                if (type == kTableFile) {
                    table_cache_->Evict(number);
                }
                Log(options_.info_log, "Delete type=%d #%lld\n", static_cast<int>(type),
                    static_cast<unsigned long long>(number));
            }
        }
    }

    // While deleting all files unblock other threads. All files being deleted
    // have unique names which will not collide with newly created files and
    // are therefore safe to delete while allowing other threads to proceed.
    // 这些需要被删除的文件，已经不会被访问到了。
    // 所以在删除期间，可以先释放锁，让其他线程能够继续执行。
    mutex_.Unlock();
    for (const std::string& filename : files_to_delete) {
        env_->RemoveFile(dbname_ + "/" + filename);
    }
    mutex_.Lock();
}

Status DBImpl::Recover(VersionEdit* edit, bool* save_manifest) {
    mutex_.AssertHeld();

    // Ignore error from CreateDir since the creation of the DB is
    // committed only when the descriptor is created, and this directory
    // may already exist from a previous failed creation attempt.
    env_->CreateDir(dbname_);
    assert(db_lock_ == nullptr);
    Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
    if (!s.ok()) {
        return s;
    }

    if (!env_->FileExists(CurrentFileName(dbname_))) {
        if (options_.create_if_missing) {
            Log(options_.info_log, "Creating DB %s since it was missing.", dbname_.c_str());
            s = NewDB();
            if (!s.ok()) {
                return s;
            }
        } else {
            return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
        }
    } else {
        if (options_.error_if_exists) {
            return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
        }
    }

    s = versions_->Recover(save_manifest);
    if (!s.ok()) {
        return s;
    }
    SequenceNumber max_sequence(0);

    // Recover from all newer log files than the ones named in the
    // descriptor (new log files may have been added by the previous
    // incarnation without registering them in the descriptor).
    //
    // Note that PrevLogNumber() is no longer used, but we pay
    // attention to it in case we are recovering a database
    // produced by an older version of leveldb.
    const uint64_t min_log = versions_->LogNumber();
    const uint64_t prev_log = versions_->PrevLogNumber();
    std::vector<std::string> filenames;
    s = env_->GetChildren(dbname_, &filenames);
    if (!s.ok()) {
        return s;
    }
    std::set<uint64_t> expected;
    versions_->AddLiveFiles(&expected);
    uint64_t number;
    FileType type;
    std::vector<uint64_t> logs;
    for (size_t i = 0; i < filenames.size(); i++) {
        if (ParseFileName(filenames[i], &number, &type)) {
            expected.erase(number);
            if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
                logs.push_back(number);
        }
    }
    if (!expected.empty()) {
        char buf[50];
        std::snprintf(buf, sizeof(buf), "%d missing files; e.g.",
                      static_cast<int>(expected.size()));
        return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
    }

    // Recover in the order in which the logs were generated
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i < logs.size(); i++) {
        s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit, &max_sequence);
        if (!s.ok()) {
            return s;
        }

        // The previous incarnation may not have written any MANIFEST
        // records after allocating this log number.  So we manually
        // update the file number allocation counter in VersionSet.
        versions_->MarkFileNumberUsed(logs[i]);
    }

    if (versions_->LastSequence() < max_sequence) {
        versions_->SetLastSequence(max_sequence);
    }

    return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log, bool* save_manifest,
                              VersionEdit* edit, SequenceNumber* max_sequence) {
    struct LogReporter : public log::Reader::Reporter {
        Env* env;
        Logger* info_log;
        const char* fname;
        Status* status;  // null if options_.paranoid_checks==false
        void Corruption(size_t bytes, const Status& s) override {
            Log(info_log, "%s%s: dropping %d bytes; %s",
                (this->status == nullptr ? "(ignoring error) " : ""), fname,
                static_cast<int>(bytes), s.ToString().c_str());
            if (this->status != nullptr && this->status->ok()) *this->status = s;
        }
    };

    mutex_.AssertHeld();

    // Open the log file
    std::string fname = LogFileName(dbname_, log_number);
    SequentialFile* file;
    Status status = env_->NewSequentialFile(fname, &file);
    if (!status.ok()) {
        MaybeIgnoreError(&status);
        return status;
    }

    // Create the log reader.
    LogReporter reporter;
    reporter.env = env_;
    reporter.info_log = options_.info_log;
    reporter.fname = fname.c_str();
    reporter.status = (options_.paranoid_checks ? &status : nullptr);
    // We intentionally make log::Reader do checksumming even if
    // paranoid_checks==false so that corruptions cause entire commits
    // to be skipped instead of propagating bad information (like overly
    // large sequence numbers).
    log::Reader reader(file, &reporter, true /*checksum*/, 0 /*initial_offset*/);
    Log(options_.info_log, "Recovering log #%llu", (unsigned long long)log_number);

    // Read all the records and add to a memtable
    std::string scratch;
    Slice record;
    WriteBatch batch;
    int compactions = 0;
    MemTable* mem = nullptr;
    while (reader.ReadRecord(&record, &scratch) && status.ok()) {
        if (record.size() < 12) {
            reporter.Corruption(record.size(), Status::Corruption("log record too small"));
            continue;
        }
        WriteBatchInternal::SetContents(&batch, record);

        if (mem == nullptr) {
            mem = new MemTable(internal_comparator_);
            mem->Ref();
        }
        status = WriteBatchInternal::InsertInto(&batch, mem);
        MaybeIgnoreError(&status);
        if (!status.ok()) {
            break;
        }
        const SequenceNumber last_seq =
            WriteBatchInternal::Sequence(&batch) + WriteBatchInternal::Count(&batch) - 1;
        if (last_seq > *max_sequence) {
            *max_sequence = last_seq;
        }

        if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
            compactions++;
            *save_manifest = true;
            status = WriteLevel0Table(mem, edit, nullptr);
            mem->Unref();
            mem = nullptr;
            if (!status.ok()) {
                // Reflect errors immediately so that conditions like full
                // file-systems cause the DB::Open() to fail.
                break;
            }
        }
    }

    delete file;

    // See if we should keep reusing the last log file.
    if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
        assert(logfile_ == nullptr);
        assert(log_ == nullptr);
        assert(mem_ == nullptr);
        uint64_t lfile_size;
        if (env_->GetFileSize(fname, &lfile_size).ok() &&
            env_->NewAppendableFile(fname, &logfile_).ok()) {
            Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
            log_ = new log::Writer(logfile_, lfile_size);
            logfile_number_ = log_number;
            if (mem != nullptr) {
                mem_ = mem;
                mem = nullptr;
            } else {
                // mem can be nullptr if lognum exists but was empty.
                mem_ = new MemTable(internal_comparator_);
                mem_->Ref();
            }
        }
    }

    if (mem != nullptr) {
        // mem did not get reused; compact it.
        if (status.ok()) {
            *save_manifest = true;
            status = WriteLevel0Table(mem, edit, nullptr);
        }
        mem->Unref();
    }

    return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base) {
    mutex_.AssertHeld();
    // 开始计时SST的构建时间，
    // 会记录到stats里。
    const uint64_t start_micros = env_->NowMicros();

    // 创建一个FileMetaData对象，用于记录SST的元数据信息。
    // 比如SST的编号、大小、最小Key、最大Key等。
    FileMetaData meta;
    // NewFileNumber()的实现很简单，就是一个自增的计数器。
    meta.number = versions_->NewFileNumber();

    // 把新SST的编号记录到pending_outputs_中，
    // 是为了告诉其他线程，这个SST正在被构建中，
    // 不要把它误删除了。
    // 比如手动Compact的过程中会检查pending_outputs_，
    // 如果待压缩的目标SST文件存在于pending_outputs_中，
    // 就终止Compact。
    pending_outputs_.insert(meta.number);

    // 创建一个MemTable的Iterator，
    // 用于读取MemTable中的所有KV数据。
    Iterator* iter = mem->NewIterator();
    Log(options_.info_log, "Level-0 table #%llu: started", (unsigned long long)meta.number);

    Status s;
    {
        mutex_.Unlock();
        // 遍历 MemTable 中的所有 KV 数据，将其写入 SSTable 文件中
        s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
        mutex_.Lock();
    }

    Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s", (unsigned long long)meta.number,
        (unsigned long long)meta.file_size, s.ToString().c_str());
    delete iter;

    // SST已经构建好了，从pending_outputs_中移除。
    pending_outputs_.erase(meta.number);

    // Note that if file_size is zero, the file has been deleted and
    // should not be added to the manifest.
    int level = 0;
    // s.ok()但是meta.file_size为0的情况很罕见，但存在。
    // 比如用户传入了特定的过滤逻辑，把MemTable的所有kv都过滤掉了，
    // 此时BuildTable成功，但是SST的内容为空。
    if (s.ok() && meta.file_size > 0) {
        const Slice min_user_key = meta.smallest.user_key();
        const Slice max_user_key = meta.largest.user_key();
        if (base != nullptr) {
            // 挑选出一个合适的 level，用于放置新的 SSTable
            level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
        }
        /* 新增了一个 SSTable，因此需要更新 VersionSet::new_files_ 字段 */
        // 新增了一个SST，把metadata记录到VersionEdit中。
        edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
    }

    // 我们可以从leveldb的log或者api接口里获取stats。
    CompactionStats stats;

    // 将Build SST的时间开销与SST大小记录到stats里
    stats.micros = env_->NowMicros() - start_micros;
    stats.bytes_written = meta.file_size;
    // 记录New SST最终被推到哪个level
    stats_[level].Add(stats);
    return s;
}

void DBImpl::CompactMemTable() {
    mutex_.AssertHeld();
    assert(imm_ != nullptr);

    // 创建一个VersionEdit对象，用于记录从当前版本到新版本的所有变化。
    // 在CompactMemTable中主要是记录新生成的SSTable文件的MetaData.
    VersionEdit edit;

    // 获取当前版本。
    // LevelDB维护了一个VersionSet，是一个version的链表。
    // 将VersionEdit Apply到base上，就可以得到一个新的version.
    Version* base = versions_->current();

    // 增加base的引用计数，防止其他线程将该version删除。
    // 比如Compact SST的过程中，会将一些version合并成一个version，
    // 并删除中间状态的version。
    base->Ref();

    // 将MemTable保存成新的SSTable文件，
    // 并将新的SSTable文件的MetaData记录到edit中。
    Status s = WriteLevel0Table(imm_, &edit, base);

    // base可以释放了。
    base->Unref();

    // 再检查下是不是在shutdown，
    // 如果在shutdown的话就及时结束当前的CompactMemTable。
    if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
        s = Status::IOError("Deleting DB during memtable compaction");
    }

    if (s.ok()) {
        // SST构建成功，
        // 把WAL相关信息记录到VersionEdit中。

        // 新的SST创建好后，旧的WAL不再需要了，
        // 所以设置为0
        edit.SetPrevLogNumber(0);
        // 设置新的SST对应的WAL编号
        edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed

        // 将VersionEdit应用到当前Version上，
        // 产生一个新的Version，加入VersionSet中，
        // 并将新的Version设置为当前Version。
        s = versions_->LogAndApply(&edit, &mutex_);
    }

    if (s.ok()) {
        // 新Version构建成功，
        // 则Compact MemTable就完成了，
        // 可以做一些清理工作了。

        // 将Immutable MemTable释放
        imm_->Unref();
        imm_ = nullptr;
        has_imm_.store(false, std::memory_order_release);

        // 移除不再需要的文件
        RemoveObsoleteFiles();
    } else {
        RecordBackgroundError(s);
    }
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
    int max_level_with_files = 1;
    {
        MutexLock l(&mutex_);
        Version* base = versions_->current();
        for (int level = 1; level < config::kNumLevels; level++) {
            if (base->OverlapInLevel(level, begin, end)) {
                max_level_with_files = level;
            }
        }
    }
    TEST_CompactMemTable();  // TODO(sanjay): Skip if memtable does not overlap
    for (int level = 0; level < max_level_with_files; level++) {
        TEST_CompactRange(level, begin, end);
    }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin, const Slice* end) {
    assert(level >= 0);
    assert(level + 1 < config::kNumLevels);

    InternalKey begin_storage, end_storage;

    ManualCompaction manual;
    manual.level = level;
    manual.done = false;
    if (begin == nullptr) {
        manual.begin = nullptr;
    } else {
        begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
        manual.begin = &begin_storage;
    }
    if (end == nullptr) {
        manual.end = nullptr;
    } else {
        end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
        manual.end = &end_storage;
    }

    MutexLock l(&mutex_);
    while (!manual.done && !shutting_down_.load(std::memory_order_acquire) && bg_error_.ok()) {
        if (manual_compaction_ == nullptr) {  // Idle
            manual_compaction_ = &manual;
            MaybeScheduleCompaction();
        } else {  // Running either my compaction or another compaction.
            background_work_finished_signal_.Wait();
        }
    }
    if (manual_compaction_ == &manual) {
        // Cancel my manual compaction since we aborted early for some reason.
        manual_compaction_ = nullptr;
    }
}

Status DBImpl::TEST_CompactMemTable() {
    // nullptr batch means just wait for earlier writes to be done
    Status s = Write(WriteOptions(), nullptr);
    if (s.ok()) {
        // Wait until the compaction completes
        MutexLock l(&mutex_);
        while (imm_ != nullptr && bg_error_.ok()) {
            background_work_finished_signal_.Wait();
        }
        if (imm_ != nullptr) {
            s = bg_error_;
        }
    }
    return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
    mutex_.AssertHeld();
    if (bg_error_.ok()) {
        bg_error_ = s;
        background_work_finished_signal_.SignalAll();
    }
}

/* Compaction 入口函数 */
void DBImpl::MaybeScheduleCompaction() {
    mutex_.AssertHeld();
    if (background_compaction_scheduled_) {
        // Already scheduled
    } else if (shutting_down_.load(std::memory_order_acquire)) {
        // DB is being deleted; no more background compactions
    } else if (!bg_error_.ok()) {
        // Already got an error; no more changes
    } else if (imm_ == nullptr && manual_compaction_ == nullptr && !versions_->NeedsCompaction()) {
        // No work to be done
    } else {
        /* 设置 background_compaction_scheduled_ 标志位，并将 BGWork
         * 方法加入线程池中 */
        background_compaction_scheduled_ = true;
        env_->Schedule(&DBImpl::BGWork, this);
    }
}

void DBImpl::BGWork(void* db) { reinterpret_cast<DBImpl*>(db)->BackgroundCall(); }

void DBImpl::BackgroundCall() {
    MutexLock l(&mutex_);
    assert(background_compaction_scheduled_);
    if (shutting_down_.load(std::memory_order_acquire)) {
        // No more background work when shutting down.
    } else if (!bg_error_.ok()) {
        // No more background work after a background error.
    } else {
        BackgroundCompaction();
    }

    background_compaction_scheduled_ = false;

    // Previous compaction may have produced too many files in a level,
    // so reschedule another compaction if needed.
    MaybeScheduleCompaction();
    background_work_finished_signal_.SignalAll();
}

void DBImpl::BackgroundCompaction() {
    mutex_.AssertHeld();

    // 先判断是否存在 Immutable MemTable，如果存在，
    // 就将本次 Compaction 判定为 MemTable Compaction。
    if (imm_ != nullptr) {
        CompactMemTable();
        return;
    }

    // 否则为判定为 SST Compaction，进入 SST Compaction 的流程。

    Compaction* c;

    bool is_manual = (manual_compaction_ != nullptr);
    InternalKey manual_end;

    // 如果是用户主动发起的手动 Compaction，本次 Compaction 的范围
    // 是由用户指定的，需要从 manual_compaction_ 中读取，而不是由 LevelDB 计算得出。
    // 否则，本次 Compaction 的范围由`PickCompaction()`计算得出。
    // 无论是手动 Compaction 还是自动 Compaction，最终都会把 Compaction 所
    // 涉及的 SST 文件编号记录到`Compaction* c`对象中。
    //    c->inputs_[0] 中存放的是 Compaction Level 所涉及的 SST 文件编号。
    //    c->inputs_[1] 中存放的是 Compaction Level+1 所涉及的 SST 文件编号。
    if (is_manual) {
        /* 手动对 SSTable 执行 Compaction，可能会造成 DB 的较大抖动 */
        ManualCompaction* m = manual_compaction_;
        c = versions_->CompactRange(m->level, m->begin, m->end);
        m->done = (c == nullptr);
        if (c != nullptr) {
            manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
        }
        Log(options_.info_log, "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
            m->level, (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
            (m->end ? m->end->DebugString().c_str() : "(end)"),
            (m->done ? "(end)" : manual_end.DebugString().c_str()));
    } else {
        /* Size Compaction 或者是 Seek Compaction */
        c = versions_->PickCompaction();
    }

    Status status;
    if (c == nullptr) {
        // Nothing to do
    } else if (!is_manual && c->IsTrivialMove()) {
        // Move file to next level
        //
        // IsTrivialMove() 表示本次 Compaction 只需要简单地将 SST 文件从
        // level 层移动到 level+1 层即可，不需要进行 SST 文件合并。

        // 在 TrivialMove 的情况下，level 层需要 Compaction 的 SST 文件
        // 只能有一个。
        assert(c->num_input_files(0) == 1);

        // 获取 level 层需要 Compaction 的第 0 个 SST 文件的元数据信息。 
        FileMetaData* f = c->input(0, 0);

        // 编辑 VersionEdit，将 level 层需要 Compact 的 SST 从 level 层移除，
        // 并将其添加到 level+1 层。
        c->edit()->RemoveFile(c->level(), f->number);
        c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest, f->largest);

        // Apply 该 VersionEdit，将其应用到当前 VersionSet 中。
        status = versions_->LogAndApply(c->edit(), &mutex_);
        if (!status.ok()) {
            RecordBackgroundError(status);
        }
        VersionSet::LevelSummaryStorage tmp;
        Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
            static_cast<unsigned long long>(f->number), c->level() + 1,
            static_cast<unsigned long long>(f->file_size), status.ToString().c_str(),
            versions_->LevelSummary(&tmp));
    } else {
        // 需要进行 SST 文件合并的 Compaction。

        // 构造一个 CompactionState 对象，用于记录本次 Compaction 
        // 需要新生成的 SST 信息。
        CompactionState* compact = new CompactionState(c);

        // 进行真正的 SST Compaction 操作，将 Compaction SST
        // 文件合并生成新的 SST 文件。
        status = DoCompactionWork(compact);
        if (!status.ok()) {
            RecordBackgroundError(status);
        }

        // Compaction 结束后的清理工作工作。
        CleanupCompaction(compact);

        // Compaction 完成后，Input Version 就不再需要了，将其释放。
        c->ReleaseInputs();

        // 移除数据库中不再需要的文件。
        RemoveObsoleteFiles();
    }
    delete c;

    if (status.ok()) {
        // Done
    } else if (shutting_down_.load(std::memory_order_acquire)) {
        // Ignore compaction errors found during shutting down
        // 
        // 如何当前正在关闭数据库，那错误就先不需要处理了，留到下次打开
        // 数据时再处理，先以最快的时间关闭数据库。
    } else {
        Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
    }

    if (is_manual) {
        ManualCompaction* m = manual_compaction_;
        if (!status.ok()) {
            // Compaction 失败了，需要把 m->done 标记为 true，
            // 防止重复 Compact 该范围。
            m->done = true;
        }
        if (!m->done) {
            // We only compacted part of the requested range.  Update *m
            // to the range that is left to be compacted.
            //
            // Compaction 完成，需要把 m->begin 更新为本次
            // Compaction 的结尾，以便下次继续 Compact。
            m->tmp_storage = manual_end;
            m->begin = &m->tmp_storage;
        }
        manual_compaction_ = nullptr;
    }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
    mutex_.AssertHeld();
    if (compact->builder != nullptr) {
        // May happen if we get a shutdown call in the middle of compaction
        compact->builder->Abandon();
        delete compact->builder;
    } else {
        assert(compact->outfile == nullptr);
    }
    delete compact->outfile;
    for (size_t i = 0; i < compact->outputs.size(); i++) {
        const CompactionState::Output& out = compact->outputs[i];
        pending_outputs_.erase(out.number);
    }
    delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
    assert(compact != nullptr);
    assert(compact->builder == nullptr);
    uint64_t file_number;
    {
        mutex_.Lock();
        file_number = versions_->NewFileNumber();
        pending_outputs_.insert(file_number);
        CompactionState::Output out;
        out.number = file_number;
        out.smallest.Clear();
        out.largest.Clear();
        compact->outputs.push_back(out);
        mutex_.Unlock();
    }

    // Make the output file
    std::string fname = TableFileName(dbname_, file_number);
    Status s = env_->NewWritableFile(fname, &compact->outfile);
    if (s.ok()) {
        compact->builder = new TableBuilder(options_, compact->outfile);
    }
    return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact, Iterator* input) {
    assert(compact != nullptr);
    assert(compact->outfile != nullptr);
    assert(compact->builder != nullptr);

    const uint64_t output_number = compact->current_output()->number;
    assert(output_number != 0);

    // Check for iterator errors
    //
    // 如果 input iterator 有错误的话，
    // 终止该 Output SST 的构建。
    Status s = input->status();
    const uint64_t current_entries = compact->builder->NumEntries();
    if (s.ok()) {
        s = compact->builder->Finish();
    } else {
        compact->builder->Abandon();
    }

    // 在 compact 对象中记录当前 SST 的文件大小。
    const uint64_t current_bytes = compact->builder->FileSize();
    compact->current_output()->file_size = current_bytes;

    // 更新本次 Compaction 所涉及的大小。
    compact->total_bytes += current_bytes;
    delete compact->builder;
    compact->builder = nullptr;

    // Finish and check for file errors
    //
    // 将该 Output SST 强制刷盘，确保数据写入磁盘，
    // 而不是停留在内核缓冲区里。
    if (s.ok()) {
        s = compact->outfile->Sync();
    }
    if (s.ok()) {
        s = compact->outfile->Close();
    }
    delete compact->outfile;
    compact->outfile = nullptr;

    // 创建一个 Iterator 来读取该 Output SST，
    // 验证其是否可用。
    if (s.ok() && current_entries > 0) {
        // Verify that the table is usable
        Iterator* iter = table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
        s = iter->status();
        delete iter;
        if (s.ok()) {
            Log(options_.info_log, "Generated table #%llu@%d: %lld keys, %lld bytes",
                (unsigned long long)output_number, compact->compaction->level(),
                (unsigned long long)current_entries, (unsigned long long)current_bytes);
        }
    }
    return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
    mutex_.AssertHeld();
    Log(options_.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
        compact->compaction->num_input_files(0), compact->compaction->level(),
        compact->compaction->num_input_files(1), compact->compaction->level() + 1,
        static_cast<long long>(compact->total_bytes));

    // Add compaction outputs
    compact->compaction->AddInputDeletions(compact->compaction->edit());
    const int level = compact->compaction->level();
    for (size_t i = 0; i < compact->outputs.size(); i++) {
        const CompactionState::Output& out = compact->outputs[i];
        compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size, out.smallest,
                                             out.largest);
    }
    return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact) {
    const uint64_t start_micros = env_->NowMicros();
    int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

    Log(options_.info_log, "Compacting %d@%d + %d@%d files",
        compact->compaction->num_input_files(0), compact->compaction->level(),
        compact->compaction->num_input_files(1), compact->compaction->level() + 1);

    assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
    assert(compact->builder == nullptr);
    assert(compact->outfile == nullptr);
    // 将当前存活着的最小 snapshot 版本号记录到`compact->smallest_snapshot`中。
    // Compaction 的时候会将 Sequence Number 小于`compact->smallest_snapshot`的
    // Key 都扔掉，因为已经没有 snapshot 需要这些 Key 了。
    if (snapshots_.empty()) {
        compact->smallest_snapshot = versions_->LastSequence();
    } else {
        compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
    }

    // 创建一个 Iterator 来读取所有的 Compaction SST。
    Iterator* input = versions_->MakeInputIterator(compact->compaction);

    // Release mutex while we're actually doing the compaction work
    mutex_.Unlock();

    input->SeekToFirst();
    Status status;
    ParsedInternalKey ikey;
    std::string current_user_key;
    bool has_current_user_key = false;
    SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
    // Compaction 的过程会比较耗时，每处理完一个 Key 就检查下是否正在关闭数据库。
    // 如果检查到正在关闭数据库，就先终止 Compaction，等下次打开数据库的时候再继续 Compaction。
    while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
        // Prioritize immutable compaction work
        // 
        // 如果检测到有 Immutable MemTable 存在，优先处理 Immutable MemTable，
        // 把 Immutable MemTable 转成 SST 文件。
        if (has_imm_.load(std::memory_order_relaxed)) {
            const uint64_t imm_start = env_->NowMicros();
            mutex_.Lock();
            if (imm_ != nullptr) {
                CompactMemTable();
                // Wake up MakeRoomForWrite() if necessary.
                background_work_finished_signal_.SignalAll();
            }
            mutex_.Unlock();
            imm_micros += (env_->NowMicros() - imm_start);
        }

        Slice key = input->key();
        // 检查本次 Compaction 的范围是否太大，如果太大的话就缩小本次的 Compaction 范围，
        // Compact 到 key 为止即可，后面的 key 等下次 Compaction 的时候再处理。
        if (compact->compaction->ShouldStopBefore(key) && compact->builder != nullptr) {
            status = FinishCompactionOutputFile(compact, input);
            if (!status.ok()) {
                break;
            }
        }

        // Handle key/value, add to state, etc.
        bool drop = false;
        if (!ParseInternalKey(key, &ikey)) {
            // Do not hide error keys
            // 
            // 如果解析 Key 失败，那可能是数据损坏的情况。
            // 把坏掉的 Key 保留下来，写到新 SST 里，让用户
            // 来处理。
            current_user_key.clear();
            has_current_user_key = false;
            last_sequence_for_key = kMaxSequenceNumber;
        } else {
            if (!has_current_user_key ||
                user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) != 0) {
                // First occurrence of this user key
                current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
                has_current_user_key = true;
                last_sequence_for_key = kMaxSequenceNumber;
            }

            // 当 key 的类型为 Deletion 时，
            // 或者 key 的 Sequence Number 小于 compact->smallest_snapshot，
            // 这个 key 就会被丢弃。
            // 举个例子:
            //    compact->smallest_snapshot = 4
            //    读到的 key 为: key1-seq5, key1-seq4, key1-seq3, key1-seq2, key1-seq1。
            //    key1-seq5 和 key1-seq4 会被保留写入到新 SST 中，
            //    剩下的 key1-seq3, key1-seq2, key1-seq1 都会被丢弃。
            if (last_sequence_for_key <= compact->smallest_snapshot) {
                // Hidden by an newer entry for same user key
                drop = true;  // (A)
            } else if (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
                       compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
                // For this user key:
                // (1) there is no data in higher levels
                // (2) data in lower levels will have larger sequence numbers
                // (3) data in layers that are being compacted here and have
                //     smaller sequence numbers will be dropped in the next
                //     few iterations of this loop (by rule (A) above).
                // Therefore this deletion marker is obsolete and can be
                // dropped.
                drop = true;
            }

            last_sequence_for_key = ikey.sequence;
        }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

        // 如果当前 key 不需要被丢弃，就把它写入到新的 SST 中。
        if (!drop) {
            // Open output file if necessary
            //
            // 如果当前没有打开的 Output SST 文件，就打开一个新的 Output SST 文件。
            // 通过 compact->builder 来控制该新 SST 文件的写入。
            if (compact->builder == nullptr) {
                status = OpenCompactionOutputFile(compact);
                if (!status.ok()) {
                    break;
                }
            }

            // 设置这个 Output SST 文件的最小 key 和最大 key。
            if (compact->builder->NumEntries() == 0) {
                compact->current_output()->smallest.DecodeFrom(key);
            }
            compact->current_output()->largest.DecodeFrom(key);

            // 往新 SST 中写入当前的 Key-Value。
            compact->builder->Add(key, input->value());

            // Close output file if it is big enough
            // 
            // Output SST 大小达到了阈值，就完成该 Ouput SST 的构建，重新开一个新的 SST。
            if (compact->builder->FileSize() >= compact->compaction->MaxOutputFileSize()) {
                status = FinishCompactionOutputFile(compact, input);
                if (!status.ok()) {
                    break;
                }
            }
        }

        input->Next();
    }

    // Compaction 和关闭数据库撞上了，终止 Compaction。
    if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
        status = Status::IOError("Deleting DB during compaction");
    }
    // 如果还有没收尾的 SST，做个收尾。
    if (status.ok() && compact->builder != nullptr) {
        status = FinishCompactionOutputFile(compact, input);
    }
    // 检查 input 迭代器有木有错误。
    if (status.ok()) {
        status = input->status();
    }
    delete input;
    input = nullptr;

    // 记录下本次 Compaction 所花费的时间。
    CompactionStats stats;
    stats.micros = env_->NowMicros() - start_micros - imm_micros;

    // 记录下本次 Compaction 一共读取了多少字节。
    for (int which = 0; which < 2; which++) {
        for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
            stats.bytes_read += compact->compaction->input(which, i)->file_size;
        }
    }

    // 记录本次 Compaction 一共写入了多少字节。
    for (size_t i = 0; i < compact->outputs.size(); i++) {
        stats.bytes_written += compact->outputs[i].file_size;
    }

    mutex_.Lock();
    // 记录本次 Compaction 被推到哪个 level.
    stats_[compact->compaction->level() + 1].Add(stats);

    // 将本次 Compaction 产生的改动(删除和新增了哪些 SST)生成一个 VersionEdit，
    // 并应用到最新的 Version 上，产生一个新的 Version。
    if (status.ok()) {
        status = InstallCompactionResults(compact);
    }
    if (!status.ok()) {
        RecordBackgroundError(status);
    }
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));
    return status;
}

namespace {

struct IterState {
    port::Mutex* const mu;
    Version* const version GUARDED_BY(mu);
    MemTable* const mem GUARDED_BY(mu);
    MemTable* const imm GUARDED_BY(mu);

    IterState(port::Mutex* mutex, MemTable* mem, MemTable* imm, Version* version)
        : mu(mutex), version(version), mem(mem), imm(imm) {}
};

static void CleanupIteratorState(void* arg1, void* arg2) {
    IterState* state = reinterpret_cast<IterState*>(arg1);
    state->mu->Lock();
    state->mem->Unref();
    if (state->imm != nullptr) state->imm->Unref();
    state->version->Unref();
    state->mu->Unlock();
    delete state;
}

}  // anonymous namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options, SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
    mutex_.Lock();
    *latest_snapshot = versions_->LastSequence();

    // Collect together all needed child iterators
    std::vector<Iterator*> list;
    list.push_back(mem_->NewIterator());
    mem_->Ref();
    if (imm_ != nullptr) {
        list.push_back(imm_->NewIterator());
        imm_->Ref();
    }
    versions_->current()->AddIterators(options, &list);
    Iterator* internal_iter = NewMergingIterator(&internal_comparator_, &list[0], list.size());
    versions_->current()->Ref();

    IterState* cleanup = new IterState(&mutex_, mem_, imm_, versions_->current());
    internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

    *seed = ++seed_;
    mutex_.Unlock();
    return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
    SequenceNumber ignored;
    uint32_t ignored_seed;
    return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
    MutexLock l(&mutex_);
    return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    Status s;
    MutexLock l(&mutex_);

    // 如果 options 中指定了 Snapshot，就使用该 Snapshot。
    // 否则的话，隐式的创建一个最新的 Snapshot，从该 Snapshot 
    // 中查询 Key。
    SequenceNumber snapshot;
    if (options.snapshot != nullptr) {
        snapshot = static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
    } else {
        snapshot = versions_->LastSequence();
    }

    // Key 可能存在 3 个地方:
    //   - MemTable
    //   - Immutable MemTable
    //   - SST
    // 这 3 个地方都需要查，先把它们的引用计数加 1，防止中途被销毁。
    MemTable* mem = mem_;
    MemTable* imm = imm_;
    Version* current = versions_->current();
    mem->Ref();
    if (imm != nullptr) imm->Ref();
    current->Ref();

    bool have_stat_update = false;
    Version::GetStats stats;

    // Unlock while reading from files and memtables
    {
        // 进入查询流程了，对 Memtable, Immutable Memtable 和 SST 只会做读取操作，
        // 不会做写操作，所以可以先释放锁，允许其他线程继续写入。
        mutex_.Unlock();
        // First look in the memtable, then in the immutable memtable (if any).
        // 
        // 基于 UserKey 和 Snapshot 构造一个 LookupKey，使用该 LookupKey 来做查询。
        LookupKey lkey(key, snapshot);
        if (mem->Get(lkey, value, &s)) {
            // Done
            // 
            // 从 MemTable 中查找成功，
            // 不会触发 Compaction。
        } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
            // Done
            //
            // 从 Immutable MemTable 中查找成功，
            // 也不会触发 Compaction。
        } else {
            // 如果查找 SST 了，有可能会触发 Compaction，
            s = current->Get(options, lkey, value, &stats);
            have_stat_update = true;
        }
        mutex_.Lock();
    }

    // 如果是从 SST 中查找的 Key，并且查找过程中存在无效查找，也就是
    // 查找了某个 SST，但是没有找到目标 Key，那么就会将该 SST 的
    // allowed_seeks 减一。当 allowed_seeks 为 0 时，将该 SST
    // 加入 Compaction 的等待列表中，并尝试触发 Compaction。
    if (have_stat_update && current->UpdateStats(stats)) {
        MaybeScheduleCompaction();
    }
    mem->Unref();
    if (imm != nullptr) imm->Unref();
    current->Unref();
    return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
    SequenceNumber latest_snapshot;
    uint32_t seed;
    Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
    return NewDBIterator(
        this, user_comparator(), iter,
        (options.snapshot != nullptr
             ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number()
             : latest_snapshot),
        seed);
}

void DBImpl::RecordReadSample(Slice key) {
    MutexLock l(&mutex_);
    if (versions_->current()->RecordReadSample(key)) {
        MaybeScheduleCompaction();
    }
}

const Snapshot* DBImpl::GetSnapshot() {
    MutexLock l(&mutex_);
    return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
    MutexLock l(&mutex_);
    snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
    return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
    return DB::Delete(options, key);
}

/* leveldb Write 实现，过程如下:
 * 1. 循环检测 leveldb 的状态，包括 Level-0 的文件个数、MemTable
 * 是否已满等信息，将在 MakeRoomForWrite() 调用中完成，该函数可能会被阻塞。
 * 2. 设置 WriteBatch 的 Sequence Number，并写入 WAL
 * 3. 写入 MemTable
 * 4. 更新 Sequence Number
 * */
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    Writer w(&mutex_);
    w.batch = updates;
    w.sync = options.sync;
    w.done = false;

    /* 此时 mutex 将会在 MutexLock 的构造函数中调用 mutex.Lock() */
    MutexLock l(&mutex_);
    writers_.push_back(&w);
    while (!w.done && &w != writers_.front()) {
        w.cv.Wait();
    }
    if (w.done) {
        return w.status;
    }

    /* 确定写入策略，写入策略与 Level-0 的 SSTable 数量有关，具体的枚举值定义在
     * dbformat.h 中
     *
     * - 当 Level-0 的文件数量达到阈值 kL0_SlowdownWritesTrigger = 8 时，会延迟
     * 1ms 写入， 等待 Level-0 向下 Compaction。本质上就是 sleep 1 毫秒。
     * - 当 Level-0 的文件数量达到阈值 kL0_StopWritesTrigger = 12
     * 时，将会停止写入，等待 Level-0 向下 Compaction */
    Status status = MakeRoomForWrite(updates == nullptr);
    /* 获取最后的 Sequence Number */
    uint64_t last_sequence = versions_->LastSequence();
    Writer* last_writer = &w;
    if (status.ok() && updates != nullptr) {  // nullptr batch is for compactions
        WriteBatch* write_batch = BuildBatchGroup(&last_writer);
        /* 将 last_sequence + 1 写入至 write_batch.rep_ 的前 8 个字节 */
        WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
        last_sequence += WriteBatchInternal::Count(write_batch);

        // Add to log and apply to memtable.  We can release the lock
        // during this phase since &w is currently responsible for logging
        // and protects against concurrent loggers and concurrent writes
        // into mem_.
        {
            mutex_.Unlock();
            /* 写入 WAL 日志 */
            status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
            bool sync_error = false;
            if (status.ok() && options.sync) {
                /* fsync 刷盘 */
                status = logfile_->Sync();
                if (!status.ok()) {
                    sync_error = true;
                }
            }
            if (status.ok()) {
                /* 写入 MemTable 中 */
                status = WriteBatchInternal::InsertInto(write_batch, mem_);
            }
            mutex_.Lock();
            if (sync_error) {
                // The state of the log file is indeterminate: the log record we
                // just added may or may not show up when the DB is re-opened.
                // So we force the DB into a mode where all future writes fail.
                RecordBackgroundError(status);
            }
        }
        if (write_batch == tmp_batch_) tmp_batch_->Clear();

        /* 更新 Sequence Number */
        versions_->SetLastSequence(last_sequence);
    }

    while (true) {
        Writer* ready = writers_.front();
        writers_.pop_front();
        if (ready != &w) {
            ready->status = status;
            ready->done = true;
            ready->cv.Signal();
        }
        if (ready == last_writer) break;
    }

    // Notify new head of write queue
    if (!writers_.empty()) {
        writers_.front()->cv.Signal();
    }

    return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-null batch
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
    mutex_.AssertHeld();
    assert(!writers_.empty());
    Writer* first = writers_.front();
    WriteBatch* result = first->batch;
    assert(result != nullptr);

    size_t size = WriteBatchInternal::ByteSize(first->batch);

    // Allow the group to grow up to a maximum size, but if the
    // original write is small, limit the growth so we do not slow
    // down the small write too much.
    size_t max_size = 1 << 20;
    if (size <= (128 << 10)) {
        max_size = size + (128 << 10);
    }

    *last_writer = first;
    std::deque<Writer*>::iterator iter = writers_.begin();
    ++iter;  // Advance past "first"
    for (; iter != writers_.end(); ++iter) {
        Writer* w = *iter;
        if (w->sync && !first->sync) {
            // Do not include a sync write into a batch handled by a non-sync
            // write.
            break;
        }

        if (w->batch != nullptr) {
            size += WriteBatchInternal::ByteSize(w->batch);
            if (size > max_size) {
                // Do not make batch too big
                break;
            }

            // Append to *result
            if (result == first->batch) {
                // Switch to temporary batch instead of disturbing caller's
                // batch
                result = tmp_batch_;
                assert(WriteBatchInternal::Count(result) == 0);
                WriteBatchInternal::Append(result, first->batch);
            }
            WriteBatchInternal::Append(result, w->batch);
        }
        *last_writer = w;
    }
    return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::MakeRoomForWrite(bool force) {
    mutex_.AssertHeld();
    assert(!writers_.empty());
    bool allow_delay = !force;
    Status s;
    while (true) {
        if (!bg_error_.ok()) {
            // Yield previous error
            s = bg_error_;
            break;
        } else if (allow_delay &&
                   versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
            /* 当 Level-0 的文件数达到阈值 kL0_SlowdownWritesTrigger = 8
             * 时，延迟 1 毫秒写入 */

            // We are getting close to hitting a hard limit on the number of
            // L0 files.  Rather than delaying a single write by several
            // seconds when we hit the hard limit, start delaying each
            // individual write by 1ms to reduce latency variance.  Also,
            // this delay hands over some CPU to the compaction thread in
            // case it is sharing the same core as the writer.
            mutex_.Unlock();
            /* Microseconds 和 Milliseconds 傻傻分不清，这里将睡眠 1 毫秒 */
            env_->SleepForMicroseconds(1000);
            allow_delay = false;  // Do not delay a single write more than once
            mutex_.Lock();
        } else if (!force && (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
            // There is room in current memtable
            /* 当前 MemTable 未满(小于等于 4MB)，可以进行写入 */
            break;
        } else if (imm_ != nullptr) {
            // We have filled up the current memtable, but the previous
            // one is still being compacted, so we wait.
            /* 等待 Immutable MemTable 刷盘 */
            Log(options_.info_log, "Current memtable full; waiting...\n");
            background_work_finished_signal_.Wait();
        } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
            // There are too many level-0 files.
            /* 当 Level-0 的文件数达到阈值 kL0_StopWritesTrigger = 12
             * 时，将停止写入 */
            Log(options_.info_log, "Too many L0 files; waiting...\n");
            background_work_finished_signal_.Wait();
        } else {
            // Attempt to switch to a new memtable and trigger compaction of old

            /* 此时表示 Immutable Memory Table 不存在，并且 Memory Table
             * 已经写满了。 那么我们需要将 MemTable 转变成 Immutable
             * MemTable，并主动触发 Compaction， 此时的 Compaction 主要是 Minor
             * Compaction */

            assert(versions_->PrevLogNumber() == 0);
            uint64_t new_log_number = versions_->NewFileNumber();
            WritableFile* lfile = nullptr;

            /* 生成新的预写日志文件 */
            s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
            if (!s.ok()) {
                // Avoid chewing through file number space in a tight loop.
                versions_->ReuseFileNumber(new_log_number);
                break;
            }
            delete log_;
            delete logfile_;
            logfile_ = lfile;
            logfile_number_ = new_log_number;
            log_ = new log::Writer(lfile);

            /* 将 MemTable 转换成 Immutable MemTable */
            imm_ = mem_;
            has_imm_.store(true, std::memory_order_release);
            /* 初始化一个新的 MemTable */
            mem_ = new MemTable(internal_comparator_);
            mem_->Ref();
            force = false;  // Do not force another compaction if have room

            /* 主动触发 Compaction */
            MaybeScheduleCompaction();
        }
    }
    return s;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
    value->clear();

    MutexLock l(&mutex_);
    Slice in = property;
    Slice prefix("leveldb.");
    if (!in.starts_with(prefix)) return false;
    in.remove_prefix(prefix.size());

    if (in.starts_with("num-files-at-level")) {
        in.remove_prefix(strlen("num-files-at-level"));
        uint64_t level;
        bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
        if (!ok || level >= config::kNumLevels) {
            return false;
        } else {
            char buf[100];
            std::snprintf(buf, sizeof(buf), "%d",
                          versions_->NumLevelFiles(static_cast<int>(level)));
            *value = buf;
            return true;
        }
    } else if (in == "stats") {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "                               Compactions\n"
                      "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
                      "--------------------------------------------------\n");
        value->append(buf);
        for (int level = 0; level < config::kNumLevels; level++) {
            int files = versions_->NumLevelFiles(level);
            if (stats_[level].micros > 0 || files > 0) {
                std::snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n", level, files,
                              versions_->NumLevelBytes(level) / 1048576.0,
                              stats_[level].micros / 1e6, stats_[level].bytes_read / 1048576.0,
                              stats_[level].bytes_written / 1048576.0);
                value->append(buf);
            }
        }
        return true;
    } else if (in == "sstables") {
        *value = versions_->current()->DebugString();
        return true;
    } else if (in == "approximate-memory-usage") {
        size_t total_usage = options_.block_cache->TotalCharge();
        if (mem_) {
            total_usage += mem_->ApproximateMemoryUsage();
        }
        if (imm_) {
            total_usage += imm_->ApproximateMemoryUsage();
        }
        char buf[50];
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(total_usage));
        value->append(buf);
        return true;
    }

    return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
    // TODO(opt): better implementation
    MutexLock l(&mutex_);
    Version* v = versions_->current();
    v->Ref();

    for (int i = 0; i < n; i++) {
        // Convert user_key into a corresponding internal key.
        InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
        InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
        uint64_t start = versions_->ApproximateOffsetOf(v, k1);
        uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
        sizes[i] = (limit >= start ? limit - start : 0);
    }

    v->Unref();
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
    WriteBatch batch;
    batch.Delete(key);
    return Write(opt, &batch);
}

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
    *dbptr = nullptr;

    DBImpl* impl = new DBImpl(options, dbname);
    impl->mutex_.Lock();
    VersionEdit edit;
    // Recover handles create_if_missing, error_if_exists
    bool save_manifest = false;
    Status s = impl->Recover(&edit, &save_manifest);
    if (s.ok() && impl->mem_ == nullptr) {
        // Create new log and a corresponding memtable.
        uint64_t new_log_number = impl->versions_->NewFileNumber();
        WritableFile* lfile;
        s = options.env->NewWritableFile(LogFileName(dbname, new_log_number), &lfile);
        if (s.ok()) {
            edit.SetLogNumber(new_log_number);
            impl->logfile_ = lfile;
            impl->logfile_number_ = new_log_number;
            impl->log_ = new log::Writer(lfile);
            impl->mem_ = new MemTable(impl->internal_comparator_);
            impl->mem_->Ref();
        }
    }
    if (s.ok() && save_manifest) {
        edit.SetPrevLogNumber(0);  // No older logs needed after recovery.
        edit.SetLogNumber(impl->logfile_number_);
        s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
    }
    if (s.ok()) {
        impl->RemoveObsoleteFiles();
        // 当数据库关闭时，可能有些还没完成的 Compaction。
        // 所以打开数据库时尝试触发一次 Compaction，检查
        // 下有没有未完成的 Compaction。
        impl->MaybeScheduleCompaction();
    }
    impl->mutex_.Unlock();
    if (s.ok()) {
        assert(impl->mem_ != nullptr);
        *dbptr = impl;
    } else {
        delete impl;
    }
    return s;
}

Snapshot::~Snapshot() = default;

Status DestroyDB(const std::string& dbname, const Options& options) {
    Env* env = options.env;
    std::vector<std::string> filenames;
    Status result = env->GetChildren(dbname, &filenames);
    if (!result.ok()) {
        // Ignore error in case directory does not exist
        return Status::OK();
    }

    FileLock* lock;
    const std::string lockname = LockFileName(dbname);
    result = env->LockFile(lockname, &lock);
    if (result.ok()) {
        uint64_t number;
        FileType type;
        for (size_t i = 0; i < filenames.size(); i++) {
            if (ParseFileName(filenames[i], &number, &type) &&
                type != kDBLockFile) {  // Lock file will be deleted at end
                Status del = env->RemoveFile(dbname + "/" + filenames[i]);
                if (result.ok() && !del.ok()) {
                    result = del;
                }
            }
        }
        env->UnlockFile(lock);  // Ignore error since state is already gone
        env->RemoveFile(lockname);
        env->RemoveDir(dbname);  // Ignore error in case dir contains other files
    }
    return result;
}

}  // namespace leveldb
