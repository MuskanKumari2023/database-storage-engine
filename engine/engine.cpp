#include "engine.h"

#include "../sstable/sstable.h"

#include <fstream>
#include <sstream>
#include <cstdio>

Engine::Engine(std::string wal_path,
    std::size_t flush_threshold,
    std::size_t compact_threshold)
: wal_path_(std::move(wal_path)),
wal_(wal_path_),
flush_threshold_(flush_threshold),
compact_threshold_(compact_threshold),
next_sstable_id_(1),
last_bloom_skips_(0) {
recoverFromWal();
}

void Engine::recoverFromWal() {
    std::ifstream check(wal_path_, std::ios::binary);
    if (!check) {
        return;
    }
    check.close();

    Memtable rebuilt;
    if (!Wal::replay(wal_path_, rebuilt)) {
        return;
    }
    memtable_ = std::move(rebuilt);
}

std::string Engine::makeSstablePath() {
    std::ostringstream oss;
    oss << "sstable_" << next_sstable_id_++ << ".sst";
    return oss.str();
}

bool Engine::flush() {
    if (memtable_.size() == 0) {
        return true;
    }

    const std::string path = makeSstablePath();
    if (!SSTableWriter::write(path, memtable_)) {
        return false;
    }

    sstable_paths_.insert(sstable_paths_.begin(), path);
    memtable_ = Memtable{};
    Wal::truncateFile(wal_path_);
    maybeCompact();
    return true;
}

void Engine::maybeFlush() {
    if (memtable_.size() >= flush_threshold_) {
        flush();
    }
}

void Engine::put(const std::string& key, const std::string& value) {
    wal_.appendPut(key, value);
    memtable_.put(key, value);
    maybeFlush();
}

std::optional<std::string> Engine::get(const std::string& key) const {
    last_bloom_skips_ = 0;

    if (memtable_.contains(key)) {
        return memtable_.get(key);
    }

    for (const auto& path : sstable_paths_) {
        SSTableReader reader(path);
        if (!reader.mayContain(key)) {
            ++last_bloom_skips_;
            continue;
        }
        if (reader.contains(key)) {
            return reader.get(key);
        }
    }

    return std::nullopt;
}

void Engine::remove(const std::string& key) {
    wal_.appendDelete(key);
    memtable_.remove(key);
    maybeFlush();
}

const Memtable& Engine::memtable() const {
    return memtable_;
}

const Wal& Engine::wal() const {
    return wal_;
}

const std::vector<std::string>& Engine::sstables() const {
    return sstable_paths_;
}

std::size_t Engine::lastBloomSkips() const {
    return last_bloom_skips_;
}

bool Engine::compact() {
    if (sstable_paths_.size() <= 1) {
        return true;
    }
    const std::string output = makeSstablePath();
    const auto old_paths = sstable_paths_;
    if (!SSTableMerger::merge(old_paths, output)) {
        return false;
    }
    for (const auto& path : old_paths) {
        std::remove(path.c_str());
    }
    sstable_paths_ = {output};
    return true;
}
void Engine::maybeCompact() {
    if (sstable_paths_.size() > compact_threshold_) {
        compact();
    }
}