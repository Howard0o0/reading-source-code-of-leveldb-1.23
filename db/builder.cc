// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"

#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {

/*
 * BuildTable 除了 Build New SSTable 之外，还会使用 meta 指针记录下 New SSTable
 * 的元信息，
 * */
Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
    Status s;
    // 初始化SST的大小为0
    meta->file_size = 0;

    // 将MemTable的迭代器指向第一个元素
    iter->SeekToFirst();

    // 生成SST的文件名
    // 在LevelDB中，SSTable的文件名的格式为/dbpath/number.sst，其中：
    // 
    //      - /dbpath/是数据库的路径，对应于dbname参数。
    //      - number是SSTable的文件编号，对应于meta->number参数。
    //      - .sst是文件的扩展名，表示这是一个SSTable文件。
    std::string fname = TableFileName(dbname, meta->number);
    if (iter->Valid()) {

        // 创建SST文件
        WritableFile* file;
        s = env->NewWritableFile(fname, &file);
        if (!s.ok()) {
            return s;
        }

        // 创建一个TableBuilder对象，
        // 用于将MemTable中的数据写入到SST文件中
        TableBuilder* builder = new TableBuilder(options, file);

        // MemTable中的kv是按照升序的，
        // 所以第一个key就是最小的key，最后一个key就是最大的key
        meta->smallest.DecodeFrom(iter->key());


        // 通过TableBuilder对象将
        // 所有kv写入到SST文件中
        Slice key;
        for (; iter->Valid(); iter->Next()) {
            key = iter->key();
            builder->Add(key, iter->value());
        }

        // 最后一个key就是最大的key
        if (!key.empty()) {
            meta->largest.DecodeFrom(key);
        }

        // Finish and check for builder errors
        // 完成收尾工作，并在metadata里记录SST的大小
        s = builder->Finish();
        if (s.ok()) {
            meta->file_size = builder->FileSize();
            assert(meta->file_size > 0);
        }
        delete builder;

        // Finish and check for file errors
        // 把buffer里剩余的数据写入到文件中
        if (s.ok()) {
            s = file->Sync();
        }
        // 关闭文件，释放文件描述符
        if (s.ok()) {
            s = file->Close();
        }
        delete file;
        file = nullptr;

        if (s.ok()) {
            // Verify that the table is usable
            // 创建一个SST的迭代器，用于检查生成的SST是否可用
            Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size);
            s = it->status();
            delete it;
        }
    }

    // Check for input iterator errors
    // 检查MemTabel的迭代器是否有错误，
    // 如果有的话，说明MemTable可能会有
    // 部分数据没有刷到SST里。
    if (!iter->status().ok()) {
        s = iter->status();
    }

    // 如果SST和MemTable都没有问题，
    // 并且该SST不为空，就保留这个SST。
    // 否则的话，删除该SST。
    // 这里可能会有些有疑惑，怎么会有s.ok()但是
    // SST为空的情况呢？
    // `builder->Add(key, iter->value()`里
    // 会检查用户是否设置了filter，如果有filter就
    // 按照用户设置的filter来过滤掉一些kv，这样
    // 就会有MemTable中的所有kv都被过滤掉的情况。
    if (s.ok() && meta->file_size > 0) {
        // Keep it
    } else {
        env->RemoveFile(fname);
    }
    return s;
}

}  // namespace leveldb
