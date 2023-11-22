// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/memtable.h"

#include "db/dbformat.h"

#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

#include "util/coding.h"

namespace leveldb {

static Slice GetLengthPrefixedSlice(const char* data) {
    uint32_t len;
    const char* p = data;
    p = GetVarint32Ptr(p, p + 5, &len);  // +5: we assume "p" is not corrupted
    return Slice(p, len);
}

/* MemTable 在初始化时 refs_ 为 0 */
MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() { assert(refs_ == 0); }

size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }

int MemTable::KeyComparator::operator()(const char* aptr,
                                        const char* bptr) const {
    // Internal keys are encoded as length-prefixed strings.
    Slice a = GetLengthPrefixedSlice(aptr);
    Slice b = GetLengthPrefixedSlice(bptr);
    return comparator.Compare(a, b);
}

// Encode a suitable internal key target for "target" and return it.
// Uses *scratch as scratch space, and the returned pointer will point
// into this scratch space.
static const char* EncodeKey(std::string* scratch, const Slice& target) {
    scratch->clear();
    PutVarint32(scratch, target.size());
    scratch->append(target.data(), target.size());
    return scratch->data();
}

class MemTableIterator : public Iterator {
   public:
    explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

    MemTableIterator(const MemTableIterator&) = delete;
    MemTableIterator& operator=(const MemTableIterator&) = delete;

    ~MemTableIterator() override = default;

    bool Valid() const override { return iter_.Valid(); }
    void Seek(const Slice& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
    void SeekToFirst() override { iter_.SeekToFirst(); }
    void SeekToLast() override { iter_.SeekToLast(); }
    void Next() override { iter_.Next(); }
    void Prev() override { iter_.Prev(); }
    Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
    Slice value() const override {
        Slice key_slice = GetLengthPrefixedSlice(iter_.key());
        return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
    }

    Status status() const override { return Status::OK(); }

   private:
    MemTable::Table::Iterator iter_;
    std::string tmp_;  // For passing to EncodeKey
};

Iterator* MemTable::NewIterator() { return new MemTableIterator(&table_); }

void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
    // MemTable::Add会将{key, value, sequence, type}编码为一个Entry, 然后插入到SkipList中 
    // MemTable Entry的格式如下:
    // |----------------------|----------------------------------|
    // | Field                | Description                      |
    // |----------------------|----------------------------------|
    // | key_size             | varint32 of internal_key.size()  |   <--- head
    // | internal_key bytes   | char[internal_key.size()]        |
    // | value_size           | varint32 of value.size()         |
    // | value bytes          | char[value.size()]               |
    // |----------------------|----------------------------------|
    // 
    // 其中, internal_key的格式如下:
    // |----------------------|----------------------------------|
    // | Field                | Description                      |
    // |----------------------|----------------------------------|
    // | user_key bytes       | char[user_key.size()]            |   <--- head
    // | sequence             | 7 Byte                           |
    // | type                 | 1 Byte                           |
    // |----------------------|----------------------------------|

    // 计算 key 和 value 的大小
    size_t key_size = key.size();
    size_t val_size = value.size();

    // InternalKey = Key + SequenceNumber(7B) + Type(1B)
    // 所以 InternalKey 的大小为 key_size + 8
    size_t internal_key_size = key_size + 8;
    
    // encoded_len是整个entry的大小
    const size_t encoded_len = VarintLength(internal_key_size) +
                               internal_key_size + VarintLength(val_size) +
                               val_size;
    
    // 从arena_中分配内存, 开辟entry的空间, 即buf
    char* buf = arena_.Allocate(encoded_len);

    // 在entry中写入internal_key_size
    char* p = EncodeVarint32(buf, internal_key_size);

    // 在entry中写入key
    std::memcpy(p, key.data(), key_size);
    p += key_size;

    // 在entry中写入sequence与type
    EncodeFixed64(p, (s << 8) | type);
    p += 8;

    // 在entry中写入value_size
    p = EncodeVarint32(p, val_size);

    // 在entry中写入value
    std::memcpy(p, value.data(), val_size);

    // 检查是否刚好将entry填满
    assert(p + val_size == buf + encoded_len);

    // 将entry插入skiplist
    table_.Insert(buf);
}

// LookupKey格式如下:
// |----------------------|-------------------------------------------------|
// | Field                | Description                                     |
// |----------------------|-------------------------------------------------|
// | internal_key_size    | varint32 of internal_key.size()                 |   <--- head
// | user_key bytes       | char[user_key.size()]                           |
// | sequence and type    | 8 bytes (SequenceNumber(7B) and ValueType(1B))  |
// |----------------------|-------------------------------------------------|
bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
    Slice memkey = key.memtable_key();
    Table::Iterator iter(&table_);
    iter.Seek(memkey.data());
    if (iter.Valid()) {
        // entry format is:
        //    klength  varint32
        //    userkey  char[klength]
        //    tag      uint64
        //    vlength  varint32
        //    value    char[vlength]
        // Check that it belongs to same user key.  We do not check the
        // sequence number since the Seek() call above should have skipped
        // all entries with overly large sequence numbers.
        const char* entry = iter.key();
        uint32_t key_length;
        const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
        if (comparator_.comparator.user_comparator()->Compare(
                Slice(key_ptr, key_length - 8), key.user_key()) == 0) {
            // Correct user key
            const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
            switch (static_cast<ValueType>(tag & 0xff)) {
                case kTypeValue: {
                    Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
                    value->assign(v.data(), v.size());
                    return true;
                }
                case kTypeDeletion:
                    *s = Status::NotFound(Slice());
                    return true;
            }
        }
    }
    return false;
}

}  // namespace leveldb
