#include "wal.h"

#include "../memtable/memtable.h"

#include <fstream>
#include <vector>

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

bool readExact(std::istream& in, std::string& out, std::size_t length) {
    out.resize(length);
    if (length == 0) {
        return true;
    }
    in.read(out.data(), static_cast<std::streamsize>(length));
    return static_cast<bool>(in);
}

}  // namespace

Wal::Wal(std::string path) : path_(std::move(path)) {}

const std::string& Wal::path() const {
    return path_;
}

bool Wal::appendRecord(WalOp op, const std::string& key, const std::string& value) {
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        return false;
    }

    const auto key_len = static_cast<std::uint32_t>(key.size());
    const auto val_len = static_cast<std::uint32_t>(value.size());

    out.put(static_cast<char>(op));
    writeU32(out, key_len);
    writeU32(out, val_len);

    if (key_len > 0) {
        out.write(key.data(), static_cast<std::streamsize>(key.size()));
    }
    if (val_len > 0) {
        out.write(value.data(), static_cast<std::streamsize>(value.size()));
    }

    out.flush(); 
    return static_cast<bool>(out);
}

bool Wal::appendPut(const std::string& key, const std::string& value) {
    return appendRecord(WalOp::Put, key, value);
}

bool Wal::appendDelete(const std::string& key) {
    return appendRecord(WalOp::Delete, key, "");
}

bool Wal::replay(const std::string& path, Memtable& memtable) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;  // no WAL yet is an error for replay(); engine handles missing file
    }

    while (true) {
        char op_byte = 0;
        in.get(op_byte);
        if (!in) {
            break;  // clean EOF
        }

        std::uint32_t key_len = 0;
        std::uint32_t val_len = 0;
        if (!readU32(in, key_len) || !readU32(in, val_len)) {
            return false;  // truncated/corrupt record
        }

        std::string key;
        std::string value;
        if (!readExact(in, key, key_len) || !readExact(in, value, val_len)) {
            return false;
        }

        const auto op = static_cast<WalOp>(static_cast<std::uint8_t>(op_byte));
        if (op == WalOp::Put) {
            memtable.put(key, value);
        } else if (op == WalOp::Delete) {
            memtable.remove(key);
        } else {
            return false;  // unknown op
        }
    }

    return true;
}

bool Wal::truncateFile(const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(out);
}