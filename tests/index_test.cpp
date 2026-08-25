#include "engine/engine.h"
#include "sstable/sstable.h"
#include "vlog/vlog.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

static void removeFile(const std::string& path) {
    std::remove(path.c_str());
}

static void test_sparse_index_seeks_near_key() {
    Memtable mt;
    for (int i = 0; i < 10; ++i) {
        mt.put("key:" + std::to_string(i), "val:" + std::to_string(i));
    }

    const std::string path = "index_seek.sst";
    removeFile(path);
    assert(SSTableWriter::write(path, mt));

    SSTableReader reader(path);
    assert(reader.get("key:9") == "val:9");
    assert(reader.get("key:0") == "val:0");
    assert(reader.get("key:5") == "val:5");
    assert(!reader.get("missing").has_value());
}

static void test_bloom_skips_negative_lookup() {
    Memtable mt;
    mt.put("apple", "1");
    mt.put("banana", "2");

    const std::string path = "index_bloom.sst";
    removeFile(path);
    assert(SSTableWriter::write(path, mt));

    SSTableReader reader(path);
    assert(!reader.mayContain("definitely_not_a_key_xyz"));
    assert(reader.mayContain("apple"));
}

static void test_engine_bloom_skips_sstables() {
    const std::string wal_path = "index_engine.wal";
    removeFile(wal_path);
    removeFile(wal_path + ".sstables");
    removeFile(ValueLog::pathFromWal(wal_path));
    removeFile(ValueLog::currentMarkerPath(ValueLog::pathFromWal(wal_path)));

    Engine engine(wal_path, 2, 10);

    engine.put("a", "1");
    engine.put("b", "2");
    engine.put("c", "3");
    engine.put("d", "4");

    assert(engine.sstables().size() >= 2);
    assert(!engine.get("not_present_anywhere").has_value());
    assert(engine.lastBloomSkips() >= 1);
}

static void test_phase4_compat() {
    Memtable mt;
    mt.put("user:1", "alice");
    mt.put("user:2", "bob");

    const std::string path = "index_compat.sst";
    removeFile(path);
    assert(SSTableWriter::write(path, mt));

    SSTableReader reader(path);
    assert(reader.get("user:1") == "alice");
    assert(reader.contains("user:2"));
    assert(!reader.get("user:99").has_value());
}

int main() {
    test_sparse_index_seeks_near_key();
    test_bloom_skips_negative_lookup();
    test_engine_bloom_skips_sstables();
    test_phase4_compat();

    std::cout << "All Phase 5 index/bloom tests passed.\n";
    return 0;
}
