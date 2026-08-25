#include "engine/engine.h"
#include "sstable/sstable.h"
#include "vlog/vlog.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void removeFile(const std::string& path) {
    std::remove(path.c_str());
}

static std::uint64_t dataSectionEnd(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return 0;
    }

    const auto file_size = in.tellg();
    if (file_size < static_cast<std::streamoff>(kSSTFooterSize)) {
        return static_cast<std::uint64_t>(file_size);
    }

    in.seekg(file_size - static_cast<std::streamoff>(kSSTFooterSize));
    std::uint64_t index_offset = 0;
    std::uint32_t magic = 0;

    for (int i = 0; i < 8; ++i) {
        char byte = 0;
        in.get(byte);
        index_offset |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(byte)) << (8 * i);
    }
    in.seekg(8, std::ios::cur);  // skip bloom_offset
    for (int shift = 0; shift < 32; shift += 8) {
        char byte = 0;
        in.get(byte);
        magic |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(byte)) << shift;
    }

    if (magic == kSSTMagic) {
        return index_offset;
    }
    return static_cast<std::uint64_t>(file_size);
}

static std::vector<std::string> readKeysInFileOrder(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> keys;
    const std::uint64_t end = dataSectionEnd(path);

    while (static_cast<std::uint64_t>(in.tellg()) < end) {
        char type_byte = 0;
        in.get(type_byte);
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

        std::string key(key_len, '\0');
        if (key_len > 0) {
            in.read(key.data(), static_cast<std::streamsize>(key_len));
        }
        in.seekg(static_cast<std::streamoff>(val_len), std::ios::cur);

        keys.push_back(key);
    }

    return keys;
}

static void test_sstable_write_sorted() {
    Memtable mt;
    mt.put("c", "3");
    mt.put("a", "1");
    mt.put("b", "2");

    const std::string path = "test_sorted.sst";
    removeFile(path);

    assert(SSTableWriter::write(path, mt));

    auto keys = readKeysInFileOrder(path);
    assert(keys.size() == 3);
    assert(keys[0] == "a");
    assert(keys[1] == "b");
    assert(keys[2] == "c");
}

static void test_sstable_reader_get() {
    Memtable mt;
    mt.put("user:1", "alice");
    mt.put("user:2", "bob");

    const std::string path = "test_reader.sst";
    removeFile(path);
    assert(SSTableWriter::write(path, mt));

    SSTableReader reader(path);
    assert(reader.get("user:1") == "alice");
    assert(reader.get("user:2") == "bob");
    assert(!reader.get("user:99").has_value());
}

static void test_engine_auto_flush() {
    const std::string wal_path = "test_flush.wal";
    removeFile(wal_path);
    removeFile(ValueLog::pathFromWal(wal_path));

    Engine engine(wal_path, /*flush_threshold=*/3);

    engine.put("k1", "v1");
    engine.put("k2", "v2");
    assert(engine.memtable().size() == 2);
    assert(engine.sstables().empty());

    engine.put("k3", "v3");  // triggers flush at threshold=3

    assert(engine.memtable().size() == 0);
    assert(engine.sstables().size() == 1);

    SSTableReader reader(engine.sstables().front());
    const auto payload = reader.get("k1");
    assert(payload.has_value());
    assert(Vptr::decode(*payload).has_value());
    assert(*payload != "v1");

    assert(engine.get("k1") == "v1");
    assert(engine.get("k2") == "v2");
    assert(engine.get("k3") == "v3");
}

static void test_tombstone_in_sstable() {
    Memtable mt;
    mt.put("a", "1");
    mt.remove("a");

    const std::string path = "test_tombstone.sst";
    removeFile(path);
    assert(SSTableWriter::write(path, mt));

    SSTableReader reader(path);
    assert(!reader.get("a").has_value());
}

int main() {
    test_sstable_write_sorted();
    test_sstable_reader_get();
    test_engine_auto_flush();
    test_tombstone_in_sstable();

    std::cout << "All SSTable tests passed.\n";
    return 0;
}
