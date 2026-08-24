#pragma once

#include "../memtable/memtable.h"
#include "../wal/wal.h"

#include <optional>
#include <string>
#include <vector>

class Engine {
public:
    explicit Engine(std::string wal_path,
        std::size_t flush_threshold = 4,
        std::size_t compact_threshold = 4);

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    void remove(const std::string& key);

    bool flush();  // manual flush; also called automatically at threshold

    const Memtable& memtable() const;
    const Wal& wal() const;
    bool compact();
    const std::vector<std::string>& sstables() const;
    std::size_t lastBloomSkips() const;

private:
    std::string wal_path_;
    Wal wal_;
    Memtable memtable_;
    std::vector<std::string> sstable_paths_;  // newest first
    std::size_t flush_threshold_;
    std::size_t compact_threshold_;
    int next_sstable_id_;
    mutable std::size_t last_bloom_skips_;

    void recoverFromWal();
    void maybeFlush();
    void maybeCompact();
    std::string makeSstablePath();
};