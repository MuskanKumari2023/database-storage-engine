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

static bool fileExists(const std::string& path) {
    std::ifstream in(path);
    return static_cast<bool>(in);
}

static void removeVlogFamily(const std::string& base) {
    removeFile(base);
    removeFile(ValueLog::currentMarkerPath(base));
    for (std::uint32_t id = 2; id <= 16; ++id) {
        removeFile(ValueLog::segmentPath(base, id));
    }
}

static void removeEngineFiles(const std::string& wal_path) {
    removeFile(wal_path);
    removeFile(wal_path + ".sstables");
    removeVlogFamily(ValueLog::pathFromWal(wal_path));
}

struct VlogRecord {
    std::string key;
    std::string value;
};

static std::vector<VlogRecord> readVlogRecords(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<VlogRecord> records;
    if (!in) {
        return records;
    }

    while (true) {
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

        VlogRecord rec;
        rec.key.resize(key_len);
        rec.value.resize(val_len);
        if (key_len > 0) {
            in.read(rec.key.data(), static_cast<std::streamsize>(key_len));
        }
        if (val_len > 0) {
            in.read(rec.value.data(), static_cast<std::streamsize>(val_len));
        }
        if (!in) {
            break;
        }
        records.push_back(std::move(rec));
    }
    return records;
}

static void test_vptr_roundtrip() {
    Vptr original{1, 42, 7};
    const std::string bytes = original.encode();
    assert(bytes.size() == kVptrEncodedSize);
    assert(static_cast<std::uint8_t>(bytes[0]) == kVptrTag);

    const auto decoded = Vptr::decode(bytes);
    assert(decoded.has_value());
    assert(decoded->segment_id == 1);
    assert(decoded->offset == 42);
    assert(decoded->value_size == 7);
    assert(!Vptr::decode("not-a-vptr").has_value());
}

static void test_vlog_append_and_read() {
    const std::string path = "test_vlog_direct.vlog";
    removeVlogFamily(path);

    ValueLog vlog(path);
    const auto a = vlog.append("k1", "v1");
    const auto b = vlog.append("k2", "v2");
    const auto c = vlog.append("k3", "hello");
    assert(a.has_value() && b.has_value() && c.has_value());
    assert(a->offset == 0);
    assert(b->offset > a->offset);
    assert(vlog.read(*a) == "v1");
    assert(vlog.read(*b) == "v2");
    assert(vlog.read(*c) == "hello");

    const auto records = readVlogRecords(path);
    assert(records.size() == 3);
    assert(records[0].key == "k1" && records[0].value == "v1");
    assert(records[1].key == "k2" && records[1].value == "v2");
    assert(records[2].key == "k3" && records[2].value == "hello");
}

static void test_engine_vlog_and_sstable_store_vptrs() {
    const std::string wal_path = "phase8_inspect.wal";
    removeEngineFiles(wal_path);

    Engine engine(wal_path, /*flush_threshold=*/3);
    engine.put("k1", "alpha");
    engine.put("k2", "beta");
    engine.put("k3", "gamma");  // flush

    const auto vlog_records = readVlogRecords(engine.vlog().path());
    assert(vlog_records.size() == 3);
    assert(vlog_records[0].key == "k1" && vlog_records[0].value == "alpha");
    assert(vlog_records[1].key == "k2" && vlog_records[1].value == "beta");
    assert(vlog_records[2].key == "k3" && vlog_records[2].value == "gamma");

    assert(engine.sstables().size() == 1);
    SSTableReader reader(engine.sstables().front());
    const auto payload = reader.get("k1");
    assert(payload.has_value());
    assert(*payload != "alpha");
    const auto vptr = Vptr::decode(*payload);
    assert(vptr.has_value());
    assert(engine.vlog().read(*vptr) == "alpha");

    assert(engine.get("k1") == "alpha");
    assert(engine.get("k2") == "beta");
    assert(engine.get("k3") == "gamma");
}

static void test_engine_get_two_step_and_crash() {
    const std::string wal_path = "phase8_crash.wal";
    removeEngineFiles(wal_path);

    {
        Engine engine(wal_path, 100, 100);
        engine.put("a", "1");
        engine.put("b", "2");
        engine.remove("b");
        engine.put("c", "3");
    }

    Engine engine(wal_path, 100, 100);
    assert(engine.get("a") == "1");
    assert(!engine.get("b").has_value());
    assert(engine.get("c") == "3");
}

static void test_segment_paths_and_current_marker() {
    const std::string base = "phase9_paths.vlog";
    removeVlogFamily(base);

    assert(ValueLog::segmentPath(base, 1) == base);
    assert(ValueLog::segmentPath(base, 2) == base + ".2");
    assert(ValueLog::currentMarkerPath(base) == base + ".current");
    assert(ValueLog::loadCurrentSegmentId(base) == kDefaultVlogSegmentId);

    assert(ValueLog::storeCurrentSegmentId(base, 3));
    assert(ValueLog::loadCurrentSegmentId(base) == 3);
}

static void test_read_other_segment() {
    const std::string base = "phase9_cross.vlog";
    removeVlogFamily(base);

    ValueLog first(base, 1);
    const auto vptr = first.append("k", "from-seg-1");
    assert(vptr.has_value());
    assert(vptr->segment_id == 1);

    ValueLog second(base, 2);
    assert(second.append("other", "x").has_value());
    assert(second.read(*vptr) == "from-seg-1");
}

static void test_gc_rewrites_live_drops_stale() {
    const std::string wal_path = "phase9_live.wal";
    removeEngineFiles(wal_path);

    Engine engine(wal_path, /*flush_threshold=*/100, /*compact_threshold=*/100);
    for (int i = 0; i < 10; ++i) {
        engine.put("k" + std::to_string(i), "v" + std::to_string(i));
    }
    for (int i = 0; i < 5; ++i) {
        engine.put("k" + std::to_string(i), "new" + std::to_string(i));
    }
    engine.remove("gone");
    engine.put("gone", "temp");
    engine.remove("gone");

    const std::string old_path = engine.vlog().path();
    assert(readVlogRecords(old_path).size() == 16);

    assert(engine.gcValueLog());
    assert(engine.vlog().segmentId() == 2);
    assert(engine.vlog().path() == ValueLog::pathFromWal(wal_path) + ".2");
    assert(!fileExists(old_path));

    const auto live = readVlogRecords(engine.vlog().path());
    assert(live.size() == 10);
    for (int i = 0; i < 5; ++i) {
        assert(engine.get("k" + std::to_string(i)) == "new" + std::to_string(i));
    }
    for (int i = 5; i < 10; ++i) {
        assert(engine.get("k" + std::to_string(i)) == "v" + std::to_string(i));
    }
    assert(!engine.get("gone").has_value());

    bool saw_old = false;
    bool saw_temp = false;
    for (const auto& rec : live) {
        if (rec.value.size() >= 1 && rec.value[0] == 'v' && rec.key.size() == 2 &&
            rec.key[1] >= '0' && rec.key[1] <= '4') {
            saw_old = true;
        }
        if (rec.key == "gone" || rec.value == "temp") {
            saw_temp = true;
        }
    }
    assert(!saw_old);
    assert(!saw_temp);
}

static void test_gc_after_flush_uses_sstable_vptrs() {
    const std::string wal_path = "phase9_flushed.wal";
    removeEngineFiles(wal_path);

    Engine engine(wal_path, /*flush_threshold=*/4, /*compact_threshold=*/100);
    engine.put("a", "1");
    engine.put("b", "2");
    engine.put("c", "3");
    engine.put("d", "4");  // flush
    engine.put("a", "1b");
    engine.put("e", "5");

    assert(engine.gcValueLog());
    assert(engine.get("a") == "1b");
    assert(engine.get("b") == "2");
    assert(engine.get("c") == "3");
    assert(engine.get("d") == "4");
    assert(engine.get("e") == "5");
    assert(readVlogRecords(engine.vlog().path()).size() == 5);
}

static void test_gc_ignores_incomplete_new_segment() {
    const std::string wal_path = "phase9_crash.wal";
    removeEngineFiles(wal_path);
    const std::string base = ValueLog::pathFromWal(wal_path);
    const std::string leftover = ValueLog::segmentPath(base, 2);

    {
        Engine engine(wal_path, 100, 100);
        engine.put("keep", "value");
        engine.put("other", "also");
    }

    {
        std::ofstream junk(leftover, std::ios::binary | std::ios::trunc);
        junk << "this is not a valid vlog";
    }

    {
        Engine engine(wal_path, 100, 100);
        assert(engine.vlog().segmentId() == 1);
        assert(engine.get("keep") == "value");
        assert(engine.get("other") == "also");
        assert(fileExists(leftover));
        assert(engine.gcValueLog());
        assert(engine.get("keep") == "value");
        assert(engine.get("other") == "also");
        assert(readVlogRecords(engine.vlog().path()).size() == 2);
    }

    assert(!fileExists(ValueLog::segmentPath(base, 1)));
    assert(ValueLog::loadCurrentSegmentId(base) == 2);

    Engine restarted(wal_path, 100, 100);
    assert(restarted.vlog().segmentId() == 2);
    assert(restarted.get("keep") == "value");
    assert(restarted.get("other") == "also");
    assert(!fileExists(ValueLog::segmentPath(base, 1)));
}

int main() {
    test_vptr_roundtrip();
    test_vlog_append_and_read();
    test_engine_vlog_and_sstable_store_vptrs();
    test_engine_get_two_step_and_crash();
    test_segment_paths_and_current_marker();
    test_read_other_segment();
    test_gc_rewrites_live_drops_stale();
    test_gc_after_flush_uses_sstable_vptrs();
    test_gc_ignores_incomplete_new_segment();

    std::cout << "All Phase 8–9 value-log tests passed.\n";
    return 0;
}
