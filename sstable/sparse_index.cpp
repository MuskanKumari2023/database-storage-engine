#include "sparse_index.h"

#include <cstring>

namespace {

constexpr std::uint32_t kIndexMagic = 0x494E4458;  // "INDX"

void writeU32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void writeU64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

bool readU32(const std::string& in, std::size_t& pos, std::uint32_t& v) {
    if (pos + 4 > in.size()) return false;
    v = static_cast<std::uint8_t>(in[pos]) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[pos + 1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[pos + 2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[pos + 3])) << 24);
    pos += 4;
    return true;
}

bool readU64(const std::string& in, std::size_t& pos, std::uint64_t& v) {
    if (pos + 8 > in.size()) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[pos + i])) << (8 * i);
    }
    pos += 8;
    return true;
}

}  // namespace

void SparseIndex::add(const std::string& key, std::uint64_t offset) {
    entries_.push_back({key, offset});
}

const std::vector<SparseIndexEntry>& SparseIndex::entries() const {
    return entries_;
}

std::uint64_t SparseIndex::seekOffsetFor(const std::string& key) const {
    if (entries_.empty()) {
        return 0;
    }

    std::size_t lo = 0;
    std::size_t hi = entries_.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (entries_[mid].key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) {
        return 0;
    }
    return entries_[lo - 1].offset;
}

std::string SparseIndex::serialize() const {
    std::string out;
    writeU32(out, kIndexMagic);
    writeU32(out, static_cast<std::uint32_t>(entries_.size()));
    for (const auto& e : entries_) {
        writeU32(out, static_cast<std::uint32_t>(e.key.size()));
        out.append(e.key);
        writeU64(out, e.offset);
    }
    return out;
}

SparseIndex SparseIndex::deserialize(const std::string& bytes) {
    SparseIndex index;
    std::size_t pos = 0;
    std::uint32_t magic = 0;
    std::uint32_t count = 0;
    if (!readU32(bytes, pos, magic) || magic != kIndexMagic ||
        !readU32(bytes, pos, count)) {
        return index;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key_len = 0;
        std::uint64_t offset = 0;
        if (!readU32(bytes, pos, key_len) || pos + key_len > bytes.size()) {
            break;
        }
        SparseIndexEntry entry;
        entry.key.assign(bytes.data() + pos, key_len);
        pos += key_len;
        if (!readU64(bytes, pos, offset)) {
            break;
        }
        index.add(entry.key, offset);
    }
    return index;
}