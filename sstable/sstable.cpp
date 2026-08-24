#include "sstable.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace {

void writeU32(std::ostream& out, std::uint32_t value) {
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
    out.put(static_cast<char>((value >> 16) & 0xFF));
    out.put(static_cast<char>((value >> 24) & 0xFF));
}

void writeU64(std::ostream& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

bool readU32(std::istream& in, std::uint32_t& value) {
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        char byte = 0;
        in.get(byte);
        if (!in) {
            return false;
        }
        value |= static_cast<std::uint8_t>(byte) << shift;
    }
    return true;
}

bool readU64(std::istream& in, std::uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        char byte = 0;
        in.get(byte);
        if (!in) {
            return false;
        }
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(byte)) << shift;
    }
    return true;
}

bool readExact(std::istream& in, std::string& out, std::size_t length) {
    out.resize(length);
    if (length == 0) {
        return true;
    }
    in.read(out.data(), static_cast<std::streamsize>(length));
    return static_cast<bool>(in);
}

bool writeEntry(std::ostream& out, const MemtableEntry& entry) {
    const auto type_byte = static_cast<std::uint8_t>(
        entry.type == EntryType::Live ? 0x01 : 0x02);
    const auto key_len = static_cast<std::uint32_t>(entry.key.size());
    const auto val_len = static_cast<std::uint32_t>(
        entry.type == EntryType::Live ? entry.value.size() : 0);

    out.put(static_cast<char>(type_byte));
    writeU32(out, key_len);
    writeU32(out, val_len);

    if (key_len > 0) {
        out.write(entry.key.data(), static_cast<std::streamsize>(entry.key.size()));
    }
    if (entry.type == EntryType::Live && val_len > 0) {
        out.write(entry.value.data(), static_cast<std::streamsize>(entry.value.size()));
    }

    return static_cast<bool>(out);
}

bool writeMetadataFooter(std::ofstream& out, SparseIndex& index, BloomFilter& bloom) {
    const auto index_offset = static_cast<std::uint64_t>(out.tellp());
    const std::string index_bytes = index.serialize();
    out.write(index_bytes.data(), static_cast<std::streamsize>(index_bytes.size()));

    const auto bloom_offset = static_cast<std::uint64_t>(out.tellp());
    const std::string bloom_bytes = bloom.serialize();
    out.write(bloom_bytes.data(), static_cast<std::streamsize>(bloom_bytes.size()));

    writeU64(out, index_offset);
    writeU64(out, bloom_offset);
    writeU32(out, kSSTMagic);
    return static_cast<bool>(out);
}

bool loadSstableFooter(const std::string& path, std::uint64_t& data_end,
                       std::uint64_t& index_offset, std::uint64_t& bloom_offset,
                       bool& has_metadata) {
    has_metadata = false;
    data_end = 0;
    index_offset = 0;
    bloom_offset = 0;

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }

    const auto file_size = in.tellg();
    if (file_size < static_cast<std::streamoff>(kSSTFooterSize)) {
        data_end = static_cast<std::uint64_t>(file_size);
        return true;
    }

    in.seekg(file_size - static_cast<std::streamoff>(kSSTFooterSize));
    std::uint32_t magic = 0;
    if (!readU64(in, index_offset) || !readU64(in, bloom_offset) || !readU32(in, magic)) {
        data_end = static_cast<std::uint64_t>(file_size);
        return true;
    }

    if (magic != kSSTMagic) {
        data_end = static_cast<std::uint64_t>(file_size);
        return true;
    }

    has_metadata = true;
    data_end = index_offset;
    return true;
}

bool readEntryAt(std::istream& in, MemtableEntry& entry, char& type_byte) {
    in.get(type_byte);
    if (!in) {
        return false;
    }

    std::uint32_t key_len = 0;
    std::uint32_t val_len = 0;
    if (!readU32(in, key_len) || !readU32(in, val_len)) {
        return false;
    }

    std::string key;
    std::string value;
    if (!readExact(in, key, key_len) || !readExact(in, value, val_len)) {
        return false;
    }

    entry.key = std::move(key);
    entry.value = std::move(value);
    entry.type = (static_cast<std::uint8_t>(type_byte) == 0x02) ? EntryType::Tombstone
                                                                : EntryType::Live;
    return true;
}

}  // namespace

bool SSTableWriter::write(const std::string& path, const Memtable& memtable) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    SparseIndex index;
    BloomFilter bloom(std::max<std::size_t>(1, memtable.size()));
    std::size_t entry_num = 0;

    memtable.forEach([&](const MemtableEntry& entry) {
        const auto offset = static_cast<std::uint64_t>(out.tellp());
        if (entry_num % kIndexInterval == 0) {
            index.add(entry.key, offset);
        }
        bloom.add(entry.key);
        writeEntry(out, entry);
        ++entry_num;
    });

    writeMetadataFooter(out, index, bloom);
    out.flush();
    return static_cast<bool>(out);
}

SSTableReader::SSTableReader(std::string path)
    : path_(std::move(path)),
      has_metadata_(false),
      data_end_(0),
      bloom_(1) {
    std::uint64_t index_offset = 0;
    std::uint64_t bloom_offset = 0;
    if (!loadSstableFooter(path_, data_end_, index_offset, bloom_offset, has_metadata_)) {
        return;
    }

    if (!has_metadata_) {
        return;
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        has_metadata_ = false;
        return;
    }

    in.seekg(static_cast<std::streamoff>(index_offset));
    std::string index_bytes(static_cast<std::size_t>(bloom_offset - index_offset), '\0');
    in.read(index_bytes.data(), static_cast<std::streamsize>(index_bytes.size()));

    in.seekg(static_cast<std::streamoff>(bloom_offset));
    const auto file_size = static_cast<std::uint64_t>(in.seekg(0, std::ios::end).tellg());
    std::string bloom_bytes(static_cast<std::size_t>(file_size - kSSTFooterSize - bloom_offset),
                            '\0');
    in.seekg(static_cast<std::streamoff>(bloom_offset));
    in.read(bloom_bytes.data(), static_cast<std::streamsize>(bloom_bytes.size()));

    index_ = SparseIndex::deserialize(index_bytes);
    bloom_ = BloomFilter::deserialize(bloom_bytes);
}

const std::string& SSTableReader::path() const {
    return path_;
}

bool SSTableReader::mayContain(const std::string& key) const {
    if (!has_metadata_) {
        return true;
    }
    return bloom_.mayContain(key);
}

bool SSTableReader::lookup(const std::string& key, bool* is_tombstone,
                           std::string* value) const {
    if (has_metadata_ && !bloom_.mayContain(key)) {
        return false;
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return false;
    }

    const std::uint64_t start = has_metadata_ ? index_.seekOffsetFor(key) : 0;
    const std::uint64_t end =
        has_metadata_ ? data_end_
                      : static_cast<std::uint64_t>(in.seekg(0, std::ios::end).tellg());

    in.seekg(static_cast<std::streamoff>(start));

    while (static_cast<std::uint64_t>(in.tellg()) < end) {
        char type_byte = 0;
        MemtableEntry entry;
        if (!readEntryAt(in, entry, type_byte)) {
            return false;
        }

        if (entry.key == key) {
            if (is_tombstone) {
                *is_tombstone = entry.type == EntryType::Tombstone;
            }
            if (value && entry.type == EntryType::Live) {
                *value = entry.value;
            }
            return true;
        }

        if (entry.key > key) {
            break;
        }
    }

    return false;
}

bool SSTableReader::contains(const std::string& key) const {
    bool is_tombstone = false;
    return lookup(key, &is_tombstone, nullptr);
}

std::optional<std::string> SSTableReader::get(const std::string& key) const {
    bool is_tombstone = false;
    std::string value;
    if (!lookup(key, &is_tombstone, &value)) {
        return std::nullopt;
    }
    if (is_tombstone) {
        return std::nullopt;
    }
    return value;
}

bool SSTableReader::lookupLinear(const std::string& key, bool* is_tombstone,
                                 std::string* value) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return false;
    }

    const std::uint64_t end =
        has_metadata_ ? data_end_
                      : static_cast<std::uint64_t>(in.seekg(0, std::ios::end).tellg());

    in.seekg(0);

    while (static_cast<std::uint64_t>(in.tellg()) < end) {
        char type_byte = 0;
        MemtableEntry entry;
        if (!readEntryAt(in, entry, type_byte)) {
            return false;
        }

        if (entry.key == key) {
            if (is_tombstone) {
                *is_tombstone = entry.type == EntryType::Tombstone;
            }
            if (value && entry.type == EntryType::Live) {
                *value = entry.value;
            }
            return true;
        }
    }

    return false;
}

std::optional<std::string> SSTableReader::getLinearScan(const std::string& key) const {
    bool is_tombstone = false;
    std::string val;
    if (!lookupLinear(key, &is_tombstone, &val)) {
        return std::nullopt;
    }
    if (is_tombstone) {
        return std::nullopt;
    }
    return val;
}

SSTableIterator::SSTableIterator(std::string path)
    : path_(std::move(path)), valid_(false), data_end_(0) {
    std::uint64_t index_offset = 0;
    std::uint64_t bloom_offset = 0;
    bool has_metadata = false;
    loadSstableFooter(path_, data_end_, index_offset, bloom_offset, has_metadata);

    in_.open(path_, std::ios::binary);
    if (in_) {
        loadNext();
    }
}

bool SSTableIterator::valid() const {
    return valid_;
}

const MemtableEntry& SSTableIterator::current() const {
    return current_;
}

bool SSTableIterator::loadNext() {
    valid_ = false;

    if (!in_ || static_cast<std::uint64_t>(in_.tellg()) >= data_end_) {
        return false;
    }

    char type_byte = 0;
    if (!readEntryAt(in_, current_, type_byte)) {
        return false;
    }

    valid_ = true;
    return true;
}

bool SSTableIterator::next() {
    return loadNext();
}

bool SSTableMerger::merge(const std::vector<std::string>& paths,
                          const std::string& output_path) {
    if (paths.empty()) {
        return false;
    }

    struct Source {
        SSTableIterator it;
        int index;

        Source(SSTableIterator iterator, int idx)
            : it(std::move(iterator)), index(idx) {}
    };

    std::vector<Source> sources;
    sources.reserve(paths.size());
    for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
        sources.emplace_back(SSTableIterator(paths[static_cast<std::size_t>(i)]), i);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    SparseIndex index;
    BloomFilter bloom(128);
    std::size_t entry_num = 0;

    while (true) {
        int min_index = -1;
        std::string min_key;

        for (const auto& src : sources) {
            if (!src.it.valid()) {
                continue;
            }
            if (min_index == -1 || src.it.current().key < min_key) {
                min_key = src.it.current().key;
                min_index = src.index;
            }
        }

        if (min_index == -1) {
            break;
        }

        MemtableEntry winner;
        bool found = false;
        int winner_source = -1;

        for (auto& src : sources) {
            if (!src.it.valid() || src.it.current().key != min_key) {
                continue;
            }
            if (!found || src.index < winner_source) {
                winner = src.it.current();
                winner_source = src.index;
                found = true;
            }
            src.it.next();
        }

        if (winner.type == EntryType::Live) {
            const auto offset = static_cast<std::uint64_t>(out.tellp());
            if (entry_num % kIndexInterval == 0) {
                index.add(winner.key, offset);
            }
            bloom.add(winner.key);
            writeEntry(out, winner);
            ++entry_num;
        }
    }

    writeMetadataFooter(out, index, bloom);
    out.flush();
    return static_cast<bool>(out);
}
