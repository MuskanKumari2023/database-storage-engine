#pragma once

#include "../memtable/memtable.h"
#include "bloom_filter.h"
#include "sparse_index.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

/*
SSTable File Format (little-endian, entries sorted by key ascending)

  Data entries:
  +--------+----------+----------+-----------+------------+
  | type   | key_len  | val_len  | key bytes | value bytes|
  | 1 byte | uint32   | uint32   | key_len   | val_len    |
  +--------+----------+----------+-----------+------------+

  type: 0x01 = Live, 0x02 = Tombstone

  Footer (20 bytes at EOF):
    index_offset  uint64
    bloom_offset  uint64
    magic         uint32  (0x53535442 = "SSTB")
*/

constexpr std::uint32_t kSSTMagic = 0x53535442;
constexpr std::size_t kIndexInterval = 2;
constexpr std::size_t kSSTFooterSize = 20;

class SSTableWriter {
public:
    static bool write(const std::string& path, const Memtable& memtable);
};

class SSTableReader {
public:
    explicit SSTableReader(std::string path);

    std::optional<std::string> get(const std::string& key) const;
    std::optional<std::string> getLinearScan(const std::string& key) const;
    bool contains(const std::string& key) const;
    bool mayContain(const std::string& key) const;

    const std::string& path() const;

private:
    std::string path_;
    bool has_metadata_;
    std::uint64_t data_end_;
    SparseIndex index_;
    BloomFilter bloom_;

    bool lookup(const std::string& key, bool* is_tombstone,
                std::string* value) const;
    bool lookupLinear(const std::string& key, bool* is_tombstone,
                      std::string* value) const;
};

class SSTableIterator {
public:
    explicit SSTableIterator(std::string path);
    SSTableIterator(SSTableIterator&&) = default;
    SSTableIterator& operator=(SSTableIterator&&) = default;
    SSTableIterator(const SSTableIterator&) = delete;
    SSTableIterator& operator=(const SSTableIterator&) = delete;

    bool valid() const;
    const MemtableEntry& current() const;
    bool next();

private:
    std::string path_;
    std::ifstream in_;
    MemtableEntry current_;
    bool valid_;
    std::uint64_t data_end_;

    bool loadNext();
};

class SSTableMerger {
public:
    // paths: newest first. Newest entry wins on duplicate keys.
    // Tombstones are omitted from output (safe when merging the full set).
    static bool merge(const std::vector<std::string>& paths,
                      const std::string& output_path);
};
