#include "engine/engine.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

static void removeFile(const std::string& path) {
    std::remove(path.c_str());
}

// Task 4.1: memtable value wins over older SSTable
static void test_memtable_wins_over_sstable() {
    const std::string wal_path = "phase4_memtable_wins.wal";
    removeFile(wal_path);

    Engine engine(wal_path, /*flush_threshold=*/2);

    engine.put("foo", "old");
    engine.put("bar", "x");  // flush → foo, bar on disk

    assert(engine.sstables().size() == 1);
    assert(engine.memtable().size() == 0);

    engine.put("foo", "new");  // foo only in memtable now

    assert(engine.get("foo") == "new");
    assert(engine.get("bar") == "x");  // from SSTable
}

// Task 4.1: memtable tombstone hides older SSTable value
static void test_memtable_tombstone_wins() {
    const std::string wal_path = "phase4_tombstone.wal";
    removeFile(wal_path);

    Engine engine(wal_path, 2);

    engine.put("foo", "old");
    engine.put("bar", "x");  // flush
    engine.remove("foo");    // tombstone in memtable

    assert(!engine.get("foo").has_value());  // must NOT return "old" from SSTable
}

// Task 4.2: key only in memtable
static void test_get_memtable_only() {
    const std::string wal_path = "phase4_memtable_only.wal";
    removeFile(wal_path);

    Engine engine(wal_path, 100);  // no flush

    engine.put("only", "memtable");
    assert(engine.get("only") == "memtable");
    assert(engine.sstables().empty());
}

// Task 4.2: key only in one SSTable (memtable empty after flush)
static void test_get_single_sstable() {
    const std::string wal_path = "phase4_single_sst.wal";
    removeFile(wal_path);

    Engine engine(wal_path, 2);

    engine.put("a", "1");
    engine.put("b", "2");  // flush

    assert(engine.memtable().size() == 0);
    assert(engine.sstables().size() == 1);
    assert(engine.get("a") == "1");
    assert(engine.get("b") == "2");
    assert(!engine.get("missing").has_value());
}

// Task 4.2: keys spread across multiple SSTables
static void test_get_multi_sstable() {
    const std::string wal_path = "phase4_multi_sst.wal";
    removeFile(wal_path);

    Engine engine(wal_path, 2);

    engine.put("a", "1");
    engine.put("b", "2");  // flush 1 → sstable_1: a, b

    engine.put("c", "3");
    engine.put("d", "4");  // flush 2 → sstable_2: c, d

    assert(engine.sstables().size() == 2);

    assert(engine.get("a") == "1");  // oldest SSTable
    assert(engine.get("b") == "2");
    assert(engine.get("c") == "3");  // newest SSTable
    assert(engine.get("d") == "4");

    engine.put("b", "updated");
    assert(engine.get("b") == "updated");  // memtable wins
}

int main() {
    test_memtable_wins_over_sstable();
    test_memtable_tombstone_wins();
    test_get_memtable_only();
    test_get_single_sstable();
    test_get_multi_sstable();

    std::cout << "All Phase 4 engine tests passed.\n";
    return 0;
}
