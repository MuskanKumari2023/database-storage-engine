#pragma once
/*
Write order on put:
  1. Append the user value to the vLog. That returns a vptr.
  2. Write the vptr to the WAL.
  3. Put the same vptr in the memtable.

If we crash after step 2, replay puts the vptr back in the memtable.
get() then reads the value from the vLog.

Delete does not write to the vLog. The old value stays on disk until GC.

vptr — 17 bytes stored in the WAL, memtable, and SSTables (little-endian)
  +------+---------------+------------+----------------+
  | tag  | segment_id    | offset     | value_size     |
  | 1    | uint32        | uint64     | uint32         |
  +------+---------------+------------+----------------+
  tag        = 0x02 (0x01 is reserved for later)
  offset     = where this record starts in the vLog file
  value_size = how long the user value is

vLog record — one key + value on disk (little-endian)
  +----------+----------+-----------+------------+
  | key_len  | val_len  | key bytes | value bytes|
  | uint32   | uint32   | key_len   | val_len    |
  +----------+----------+-----------+------------+
  We store the key so GC can ask: "does the LSM still point here?"

Files (example base name test.vlog):
  test.vlog          first file (segment 1)
  test.vlog.2        second file (segment 2), created by GC
  test.vlog.current  one number: which file is active (missing means 1)

GC — copy values we still need, throw away the rest:
  1. Create an empty new file (test.vlog.2).
  2. Read the old file. For each record, look up the key in the LSM.
     If the LSM still points at this exact record, copy it to the new
     file and store the new vptr in the WAL and memtable.
     If the key was overwritten or deleted, skip the record.
  3. Write test.vlog.current = 2. After this, we open the new file.
     If we crash before this write, we still open the old file.
  4. Flush and compact so SSTables also hold the new vptrs.
  5. Delete the old file.
*/

#include <cstdint>
#include <functional>
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

struct VlogScanEntry {
    std::uint64_t offset = 0;
    std::string key;
    std::string value;
};

class ValueLog {
public:
    explicit ValueLog(std::string base_path,
                      std::uint32_t segment_id = kDefaultVlogSegmentId);

    static std::string pathFromWal(const std::string& wal_path);
    static std::string segmentPath(const std::string& base_path, std::uint32_t segment_id);
    static std::string currentMarkerPath(const std::string& base_path);
    static std::uint32_t loadCurrentSegmentId(const std::string& base_path);
    static bool storeCurrentSegmentId(const std::string& base_path, std::uint32_t segment_id);

    std::optional<Vptr> append(const std::string& key, const std::string& value);
    std::optional<std::string> read(const Vptr& vptr) const;
    void forEachRecord(const std::function<void(const VlogScanEntry&)>& fn) const;
    bool truncate();

    const std::string& path() const;
    const std::string& basePath() const;
    std::uint32_t segmentId() const;
    std::uint64_t nextOffset() const;

private:
    std::string base_path_;
    std::string path_;
    std::uint32_t segment_id_;
    std::uint64_t next_offset_;
};
