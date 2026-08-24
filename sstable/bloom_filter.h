#pragma once

#include <cstdint>
#include <string>
#include <vector>

class BloomFilter {
public:
    BloomFilter(std::size_t expected_items, double false_positive_rate = 0.01);

    void add(const std::string& key);
    bool mayContain(const std::string& key) const;

    std::string serialize() const;
    static BloomFilter deserialize(const std::string& bytes);

    std::size_t numBits() const;

private:
    std::vector<std::uint8_t> bits_;
    std::size_t num_bits_;
    std::size_t num_hashes_;

    std::vector<std::size_t> hashPositions(const std::string& key) const;
};