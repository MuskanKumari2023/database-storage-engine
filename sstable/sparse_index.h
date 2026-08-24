#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SparseIndexEntry {
    std::string key;
    std::uint64_t offset;
};

class SparseIndex {
public:
    void add(const std::string& key, std::uint64_t offset);

    // Largest indexed key <= search key, or 0 if none.
    std::uint64_t seekOffsetFor(const std::string& key) const;

    const std::vector<SparseIndexEntry>& entries() const;

    std::string serialize() const;
    static SparseIndex deserialize(const std::string& bytes);

private:
    std::vector<SparseIndexEntry> entries_;
};