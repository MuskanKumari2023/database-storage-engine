#include "memtable/memtable.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static void test_put_get() {
    Memtable mt;

    mt.put("user:1", "alice");
    mt.put("user:3", "carol");
    mt.put("user:2", "bob");
    mt.put("user:4", "dave");
    mt.put("user:5", "eve");

    assert(mt.get("user:1") == "alice");
    assert(mt.get("user:2") == "bob");
    assert(mt.get("user:3") == "carol");
    assert(mt.get("user:4") == "dave");
    assert(mt.get("user:5") == "eve");
    assert(!mt.get("user:99").has_value());

    mt.put("user:2", "BOB_UPDATED");
    assert(mt.get("user:2") == "BOB_UPDATED");
    assert(mt.size() == 5);
}

static void test_tombstones_and_iteration() {
    Memtable mt;

    mt.put("a", "1");
    mt.put("b", "2");
    mt.put("c", "3");

    mt.remove("b");
    assert(!mt.get("b").has_value());

    std::vector<std::string> keys;
    mt.forEach([&](const MemtableEntry& entry) {
        keys.push_back(entry.key);
        if (entry.key == "b") {
            assert(entry.type == EntryType::Tombstone);
        }
    });

    assert(keys.size() == 3);
    assert(keys[0] == "a");
    assert(keys[1] == "b");
    assert(keys[2] == "c");

    mt.put("b", "TWO");
    assert(mt.get("b") == "TWO");

    mt.forEach([&](const MemtableEntry& entry) {
        if (entry.key == "b") {
            assert(entry.type == EntryType::Live);
            assert(entry.value == "TWO");
        }
    });
}

static void test_sorted_insert_stress() {
    Memtable mt;

    for (int i = 0; i < 100; ++i) {
        mt.put("key:" + std::to_string(i), "val:" + std::to_string(i));
    }

    for (int i = 0; i < 100; ++i) {
        assert(mt.get("key:" + std::to_string(i)) ==
               "val:" + std::to_string(i));
    }
}

int main() {
    test_put_get();
    test_tombstones_and_iteration();
    test_sorted_insert_stress();

    std::cout << "All memtable tests passed.\n";
    return 0;
}
