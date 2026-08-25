#include "engine/engine.h"
#include "wal/wal.h"
#include "vlog/vlog.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static void removeFile(const std::string& path) {
    std::remove(path.c_str());
}

static std::size_t countWalRecords(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return 0;
    }

    std::size_t count = 0;
    while (true) {
        char op = 0;
        in.get(op);
        if (!in) {
            break;
        }

        char len_buf[8];
        in.read(len_buf, 8);
        if (!in) {
            break;
        }

        std::uint32_t key_len = static_cast<std::uint8_t>(len_buf[0]) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len_buf[1])) << 8) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len_buf[2])) << 16) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len_buf[3])) << 24);

        std::uint32_t val_len = static_cast<std::uint8_t>(len_buf[4]) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len_buf[5])) << 8) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len_buf[6])) << 16) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len_buf[7])) << 24);

        in.seekg(static_cast<std::streamoff>(key_len + val_len), std::ios::cur);
        if (!in) {
            break;
        }
        ++count;
    }
    return count;
}

static void test_wal_append_five_records() {
    const std::string path = "test.wal";
    removeFile(path);

    Wal wal(path);
    assert(wal.appendPut("k1", "v1"));
    assert(wal.appendPut("k2", "v2"));
    assert(wal.appendPut("k3", "v3"));
    assert(wal.appendDelete("k2"));
    assert(wal.appendPut("k4", "v4"));

    assert(countWalRecords(path) == 5);
}

static void test_replay_into_memtable() {
    const std::string path = "test_replay.wal";
    removeFile(path);

    {
        Wal wal(path);
        wal.appendPut("user:1", "alice");
        wal.appendPut("user:2", "bob");
        wal.appendDelete("user:2");
        wal.appendPut("user:3", "carol");
    }

    Memtable mt;
    assert(Wal::replay(path, mt));

    assert(mt.get("user:1") == "alice");
    assert(!mt.get("user:2").has_value());
    assert(mt.get("user:3") == "carol");
}

static void test_engine_crash_recovery() {
    const std::string path = "test_engine.wal";
    removeFile(path);
    removeFile(path + ".sstables");
    removeFile(ValueLog::pathFromWal(path));
    removeFile(ValueLog::currentMarkerPath(ValueLog::pathFromWal(path)));

    // "Process 1" writes data
    // High flush threshold so data stays in WAL for crash recovery test
    {
        Engine engine(path, 100, 100);
        engine.put("a", "1");
        engine.put("b", "2");
        engine.put("c", "3");
        engine.remove("b");
        engine.put("d", "4");
    }

    // "Process 2" simulates restart after crash
    Engine engine(path, 100, 100);
    assert(engine.get("a") == "1");
    assert(!engine.get("b").has_value());
    assert(engine.get("c") == "3");
    assert(engine.get("d") == "4");
}

int main() {
    test_wal_append_five_records();
    test_replay_into_memtable();
    test_engine_crash_recovery();

    std::cout << "All WAL tests passed.\n";
    return 0;
}
