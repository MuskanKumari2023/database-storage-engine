#pragma once
/*
WAL Record Format (little-endian, append-only)
  +--------+----------+----------+-----------+------------+
  | op (1) | key_len  | val_len  | key bytes | value bytes|
  | byte   | uint32   | uint32   | key_len   | val_len    |
  +--------+----------+----------+-----------+------------+
  op:  0x01 = PUT,  0x02 = DELETE
  DELETE: val_len = 0, no value bytes
  Records are concatenated back-to-back until EOF.
*/

#include <cstdint>
#include <string>

class Memtable;

enum class WalOp : std::uint8_t {
    Put = 0x01,
    Delete = 0x02,
};

// Append-only write-ahead log.
// Spec: see comment block at top of wal.cpp
class Wal {
public:
    explicit Wal(std::string path);

    bool appendPut(const std::string& key, const std::string& value);
    bool appendDelete(const std::string& key);

    static bool replay(const std::string& path, Memtable& memtable);
    static bool truncateFile(const std::string& path);  // used after flush in Phase 3

    const std::string& path() const;

private:
    std::string path_;

    bool appendRecord(WalOp op, const std::string& key, const std::string& value);
};