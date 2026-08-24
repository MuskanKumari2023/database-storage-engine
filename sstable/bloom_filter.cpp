#include "bloom_filter.h"

#include <cmath>
#include <cstring>

namespace {

constexpr std::uint32_t kBloomMagic = 0x424C4F4D;  // "BLOM"

std::uint64_t hash1(const std::string& key) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::uint64_t hash2(const std::string& key) {
    std::uint64_t h = 0;
    for (unsigned char c : key) {
        h = h * 131 + c;
    }
    return h;
}

void writeU32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
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

}  // namespace

BloomFilter::BloomFilter(std::size_t expected_items, double false_positive_rate) {
    if (expected_items == 0) {
        expected_items = 1;
    }
    const double m = -static_cast<double>(expected_items) *
                     std::log(false_positive_rate) / (std::log(2) * std::log(2));
    num_bits_ = std::max<std::size_t>(64, static_cast<std::size_t>(m));
    num_hashes_ = std::max<std::size_t>(
        1, static_cast<std::size_t>((num_bits_ / static_cast<double>(expected_items)) * std::log(2)));
    bits_.assign((num_bits_ + 7) / 8, 0);
}

void BloomFilter::add(const std::string& key) {
    for (std::size_t pos : hashPositions(key)) {
        bits_[pos / 8] |= static_cast<std::uint8_t>(1u << (pos % 8));
    }
}

bool BloomFilter::mayContain(const std::string& key) const {
    for (std::size_t pos : hashPositions(key)) {
        if ((bits_[pos / 8] & static_cast<std::uint8_t>(1u << (pos % 8))) == 0) {
            return false;
        }
    }
    return true;
}

std::size_t BloomFilter::numBits() const {
    return num_bits_;
}

std::vector<std::size_t> BloomFilter::hashPositions(const std::string& key) const {
    const std::uint64_t h1 = hash1(key);
    const std::uint64_t h2 = hash2(key);
    std::vector<std::size_t> positions;
    positions.reserve(num_hashes_);
    for (std::size_t i = 0; i < num_hashes_; ++i) {
        positions.push_back((h1 + i * h2) % num_bits_);
    }
    return positions;
}

std::string BloomFilter::serialize() const {
    std::string out;
    out.reserve(12 + bits_.size());
    writeU32(out, kBloomMagic);
    writeU32(out, static_cast<std::uint32_t>(num_bits_));
    writeU32(out, static_cast<std::uint32_t>(num_hashes_));
    out.append(reinterpret_cast<const char*>(bits_.data()),
               static_cast<std::size_t>(bits_.size()));
    return out;
}

BloomFilter BloomFilter::deserialize(const std::string& bytes) {
    std::size_t pos = 0;
    std::uint32_t magic = 0;
    std::uint32_t num_bits = 0;
    std::uint32_t num_hashes = 0;
    if (!readU32(bytes, pos, magic) || magic != kBloomMagic ||
        !readU32(bytes, pos, num_bits) || !readU32(bytes, pos, num_hashes)) {
        return BloomFilter(1);
    }

    BloomFilter bf(1);
    bf.num_bits_ = num_bits;
    bf.num_hashes_ = num_hashes;
    bf.bits_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end());
    return bf;
}