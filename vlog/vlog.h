#pragma once
/*

Decision :
  Append the user value to the vLog FIRST, then store the encoded vptr in
  the WAL and memtable. The vLog is write-ahead for values; the WAL is
  write-ahead for the LSM pointer.
  Crash after WAL: replay restores the vptr; get() reads the vLog.

vptr (17 bytes, little-endian) — LSM / WAL / SSTable payload
  +------+---------------+------------+----------------+
  | tag  | segment_id    | offset     | value_size     |
  | 1    | uint32        | uint64     | uint32         |
  +------+---------------+------------+----------------+
  tag = 0x02 (kVptrTag). Leaves 0x01 for a future inline-small-value tag.
  offset     = byte position of the start of the vLog record
  value_size = length of the user value bytes inside that record
  Phase 8 uses a single segment (segment_id = 1).

vLog record (append-only, little-endian)
  +----------+----------+-----------+------------+
  | key_len  | val_len  | key bytes | value bytes|
  | uint32   | uint32   | key_len   | val_len    |
  +----------+----------+-----------+------------+
  Key is stored so GC can ask the LSM "is this still the live vptr?"
  No checksum in v1; a CRC can be added in a later format bump.

Deletes do not append to the vLog. The LSM tombstone hides the old vptr;
the old record stays until GC.
*/

#include <cstdint>
#include <optional>
#include <string>

constexpr std::uint8_t kVptrTag = 0x02;
constexpr std::size_t kVptrEncodedSize = 17;
constexpr std::uint32_t kDefaultVlogSegmentId = 1;

struct Vptr {
    std::uint32_t segment_id = kDefaultVlogSegmentId;
    std::uint64_t offset = 0;
    std::uint32_t value_size = 0;

    std::string encode() const;
    static std::optional<Vptr> decode(const std::string& bytes);
};

class ValueLog {
public:
    explicit ValueLog(std::string path,
                      std::uint32_t segment_id = kDefaultVlogSegmentId);

    static std::string pathFromWal(const std::string& wal_path);

    std::optional<Vptr> append(const std::string& key, const std::string& value);
    std::optional<std::string> read(const Vptr& vptr) const;

    const std::string& path() const;
    std::uint32_t segmentId() const;
    std::uint64_t nextOffset() const;

private:
    std::string path_;
    std::uint32_t segment_id_;
    std::uint64_t next_offset_;
};
