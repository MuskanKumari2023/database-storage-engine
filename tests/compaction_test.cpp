#include "engine/engine.h"
#include "sstable/sstable.h"
#include "vlog/vlog.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

static void removeFile(const std::string& path) {
    std::remove(path.c_str());
}

// Task 6.1: merge 2 SSTables, newer value wins
static void test_merge_two_overlapping() {
    Memtable older;
    older.put("a", "old");
    older.put("b", "from_old");

    Memtable newer;
    newer.put("a", "new");
    newer.put("c", "from_new");

    const std::string path_old = "merge_old.sst";
    const std::string path_new = "merge_new.sst";
    const std::string path_out = "merge_out.sst";

    removeFile(path_old);
    removeFile(path_new);
    removeFile(path_out);

    assert(SSTableWriter::write(path_old, older));
    assert(SSTableWriter::write(path_new, newer));

    // newest first
    assert(SSTableMerger::merge({path_new, path_old}, path_out));

    SSTableReader reader(path_out);
    assert(reader.get("a") == "new");
    assert(reader.get("b") == "from_old");
    assert(reader.get("c") == "from_new");
}

// Task 6.1: tombstone in newer SSTable wins over live in older
static void test_merge_tombstone_wins() {
    Memtable older;
    older.put("x", "live");

    Memtable newer;
    newer.put("x", "ignored");
    newer.remove("x");

    const std::string path_old = "merge_ts_old.sst";
    const std::string path_new = "merge_ts_new.sst";
    const std::string path_out = "merge_ts_out.sst";

    removeFile(path_old);
    removeFile(path_new);
    removeFile(path_out);

    assert(SSTableWriter::write(path_old, older));
    assert(SSTableWriter::write(path_new, newer));
    assert(SSTableMerger::merge({path_new, path_old}, path_out));

    SSTableReader reader(path_out);
    assert(!reader.contains("x"));  // tombstone dropped from merged output
}

// Task 6.2: auto-compact when SSTable count exceeds threshold
static void test_engine_auto_compact() {
    const std::string wal_path = "compact_auto.wal";
    removeFile(wal_path);
    removeFile(wal_path + ".sstables");
    removeFile(ValueLog::pathFromWal(wal_path));
    removeFile(ValueLog::currentMarkerPath(ValueLog::pathFromWal(wal_path)));

    Engine engine(wal_path, /*flush_threshold=*/2, /*compact_threshold=*/3);

    engine.put("k1", "v1");
    engine.put("k2", "v2");  // flush 1

    engine.put("k3", "v3");
    engine.put("k4", "v4");  // flush 2

    engine.put("k5", "v5");
    engine.put("k6", "v6");  // flush 3 → 3 SSTables

    engine.put("k7", "v7");
    engine.put("k8", "v8");  // flush 4 → 4 SSTables → compact → 1

    assert(engine.sstables().size() == 1);
    assert(engine.get("k1") == "v1");
    assert(engine.get("k4") == "v4");
    assert(engine.get("k8") == "v8");
    assert(!engine.get("missing").has_value());
}

// Overwrite across SSTables + memtable after compact
static void test_compact_preserves_latest() {
    const std::string wal_path = "compact_latest.wal";
    removeFile(wal_path);
    removeFile(wal_path + ".sstables");
    removeFile(ValueLog::pathFromWal(wal_path));
    removeFile(ValueLog::currentMarkerPath(ValueLog::pathFromWal(wal_path)));

    Engine engine(wal_path, 2, 3);

    engine.put("foo", "v1");
    engine.put("bar", "x");   // flush
    engine.put("foo", "v2");
    engine.put("baz", "y");   // flush
    engine.put("foo", "v3");  // memtable

    assert(engine.compact());  // manual compact all SSTables

    assert(engine.get("foo") == "v3");  // memtable wins
    assert(engine.get("bar") == "x");
    assert(engine.get("baz") == "y");
}

int main() {
    test_merge_two_overlapping();
    test_merge_tombstone_wins();
    test_engine_auto_compact();
    test_compact_preserves_latest();

    std::cout << "All compaction tests passed.\n";
    return 0;
}
