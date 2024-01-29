
#include <cassert>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/filter_policy.h"

std::string decimalTo62(long long n) {
    char characters[] =
        "0123456789abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    std::string result;

    while (n) {
        result.push_back(characters[n % 62]);
        n = n / 62;
    }

    while (result.size() < 6) {
        result.push_back('0');
    }

    reverse(result.begin(), result.end());
    return result;
}

void putData(leveldb::DB* db, leveldb::WriteOptions* writeOptions, int keyCount, int init,
             int steps) {
    int decimal = init;
    while (keyCount > 0) {
        std::string key = decimalTo62(decimal);
        std::string value = key + key;
        db->Put(*writeOptions, key, value);
        decimal += steps;
        keyCount--;
    }
}

#include "leveldb/db.h"
#include "leveldb/write_batch.h"

void debugManualCompaction() {
    leveldb::DB* db;
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);

    if (!status.ok()) {
        std::cerr << "Open DB failed: " << status.ToString() << std::endl;
        return;
    }

    // Insert 100 keys
    for (int i = 0; i < 100; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        db->Put(leveldb::WriteOptions(), key, value);
    }

    // Trigger manual compaction for keys from "key10" to "key50"
    leveldb::Slice start_key = "key10";
    leveldb::Slice end_key = "key50";
    db->CompactRange(&start_key, &end_key);

    delete db;
}

void insertKeys() {

    leveldb::DB* db;
    leveldb::Options options;

    options.create_if_missing = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.block_size = 20 * 1024; // 设置Data Block大小为8KB

    leveldb::Status status = leveldb::DB::Open(options, "/tmp/leveldb", &db);

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;

    int numThreads = 1;
    int total = 2048;

    std::vector<std::thread> threads(numThreads);

    for (int i = 0; i < numThreads; i++) {
        threads[i] = std::thread(putData, db, &writeOptions, total / numThreads, i, numThreads);
    }

    for (auto& t : threads) {
        t.join();
    }
}

void debugGetKeyValue() {

    leveldb::DB* db;
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);

    if (!status.ok()) {
        std::cerr << "Open DB failed: " << status.ToString() << std::endl;
        return;
    }

    // Insert 100 keys
    for (int i = 0; i < 100; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        db->Put(leveldb::WriteOptions(), key, value);
    }

    leveldb::Slice key("key10");
    std::string value;
    db->Get(leveldb::ReadOptions(), key, &value);
}

int main() {

    // debugManualCompaction();
    debugGetKeyValue();
    return 0;
}
