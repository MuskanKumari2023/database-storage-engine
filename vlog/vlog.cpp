#include "vlog.h"

#include <fstream>
#include <sstream>

namespace {

void writeU32(std::ostream& out, std::uint32_t value) {
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
    out.put(static_cast<char>((value >> 16) & 0xFF));
    out.put(static_cast<char>((value >> 24) & 0xFF));
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

void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void appendU64(std::string& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

bool readU32At(const std::string& in, std::size_t pos, std::uint32_t& value) {
    if (pos + 4 > in.size()) {
        return false;
    }
    value = static_cast<std::uint8_t>(in[pos]) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[pos + 1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[pos + 2])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[pos + 3])) << 24);
    return true;
}

bool readU64At(const std::string& in, std::size_t pos, std::uint64_t& value) {
    if (pos + 8 > in.size()) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[pos + i])) << (8 * i);
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

}  // namespace

std::string Vptr::encode() const {
    std::string out;
    out.reserve(kVptrEncodedSize);
    out.push_back(static_cast<char>(kVptrTag));
    appendU32(out, segment_id);
    appendU64(out, offset);
    appendU32(out, value_size);
    return out;
}

std::optional<Vptr> Vptr::decode(const std::string& bytes) {
    if (bytes.size() != kVptrEncodedSize) {
        return std::nullopt;
    }
    if (static_cast<std::uint8_t>(bytes[0]) != kVptrTag) {
        return std::nullopt;
    }
    Vptr vptr;
    if (!readU32At(bytes, 1, vptr.segment_id) ||
        !readU64At(bytes, 5, vptr.offset) ||
        !readU32At(bytes, 13, vptr.value_size)) {
        return std::nullopt;
    }
    return vptr;
}

ValueLog::ValueLog(std::string base_path, std::uint32_t segment_id)
    : base_path_(std::move(base_path)),
      path_(segmentPath(base_path_, segment_id)),
      segment_id_(segment_id),
      next_offset_(0) {
    std::ifstream in(path_, std::ios::binary | std::ios::ate);
    if (in) {
        const auto end = in.tellg();
        if (end > 0) {
            next_offset_ = static_cast<std::uint64_t>(end);
        }
    }
}

std::string ValueLog::pathFromWal(const std::string& wal_path) {
    const std::string suffix = ".wal";
    if (wal_path.size() >= suffix.size() &&
        wal_path.compare(wal_path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return wal_path.substr(0, wal_path.size() - suffix.size()) + ".vlog";
    }
    return wal_path + ".vlog";
}

std::string ValueLog::segmentPath(const std::string& base_path, std::uint32_t segment_id) {
    if (segment_id <= 1) {
        return base_path;
    }
    std::ostringstream oss;
    oss << base_path << '.' << segment_id;
    return oss.str();
}

std::string ValueLog::currentMarkerPath(const std::string& base_path) {
    return base_path + ".current";
}

std::uint32_t ValueLog::loadCurrentSegmentId(const std::string& base_path) {
    std::ifstream in(currentMarkerPath(base_path));
    if (!in) {
        return kDefaultVlogSegmentId;
    }
    std::uint32_t id = kDefaultVlogSegmentId;
    in >> id;
    if (!in || id == 0) {
        return kDefaultVlogSegmentId;
    }
    return id;
}

bool ValueLog::storeCurrentSegmentId(const std::string& base_path, std::uint32_t segment_id) {
    std::ofstream out(currentMarkerPath(base_path), std::ios::trunc);
    if (!out) {
        return false;
    }
    out << segment_id << '\n';
    out.flush();
    return static_cast<bool>(out);
}

const std::string& ValueLog::path() const {
    return path_;
}

const std::string& ValueLog::basePath() const {
    return base_path_;
}

std::uint32_t ValueLog::segmentId() const {
    return segment_id_;
}

std::uint64_t ValueLog::nextOffset() const {
    return next_offset_;
}

std::optional<Vptr> ValueLog::append(const std::string& key, const std::string& value) {
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        return std::nullopt;
    }

    const auto key_len = static_cast<std::uint32_t>(key.size());
    const auto val_len = static_cast<std::uint32_t>(value.size());
    const Vptr vptr{segment_id_, next_offset_, val_len};

    writeU32(out, key_len);
    writeU32(out, val_len);
    if (key_len > 0) {
        out.write(key.data(), static_cast<std::streamsize>(key.size()));
    }
    if (val_len > 0) {
        out.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    out.flush();
    if (!out) {
        return std::nullopt;
    }

    next_offset_ += 8ull + static_cast<std::uint64_t>(key_len) +
                    static_cast<std::uint64_t>(val_len);
    return vptr;
}

std::optional<std::string> ValueLog::read(const Vptr& vptr) const {
    const std::string file = (vptr.segment_id == segment_id_)
                                 ? path_
                                 : segmentPath(base_path_, vptr.segment_id);

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    in.seekg(static_cast<std::streamoff>(vptr.offset));
    std::uint32_t key_len = 0;
    std::uint32_t val_len = 0;
    if (!readU32(in, key_len) || !readU32(in, val_len)) {
        return std::nullopt;
    }
    if (val_len != vptr.value_size) {
        return std::nullopt;
    }

    std::string key;
    std::string value;
    if (!readExact(in, key, key_len) || !readExact(in, value, val_len)) {
        return std::nullopt;
    }
    return value;
}

void ValueLog::forEachRecord(const std::function<void(const VlogScanEntry&)>& fn) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return;
    }

    std::uint64_t offset = 0;
    while (true) {
        std::uint32_t key_len = 0;
        std::uint32_t val_len = 0;
        if (!readU32(in, key_len) || !readU32(in, val_len)) {
            break;
        }
        VlogScanEntry entry;
        entry.offset = offset;
        if (!readExact(in, entry.key, key_len) || !readExact(in, entry.value, val_len)) {
            break;
        }
        fn(entry);
        offset += 8ull + static_cast<std::uint64_t>(key_len) +
                  static_cast<std::uint64_t>(val_len);
    }
}

bool ValueLog::truncate() {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    next_offset_ = 0;
    return true;
}
