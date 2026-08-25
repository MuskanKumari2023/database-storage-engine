#include "engine.h"

#include "../sstable/sstable.h"

#include <fstream>
#include <sstream>
#include <cstdio>

Engine::Engine(std::string wal_path,
    std::size_t flush_threshold,
    std::size_t compact_threshold)
: wal_path_(std::move(wal_path)),
wal_(wal_path_),
vlog_(ValueLog::pathFromWal(wal_path_),
      ValueLog::loadCurrentSegmentId(ValueLog::pathFromWal(wal_path_))),
flush_threshold_(flush_threshold),
compact_threshold_(compact_threshold),
next_sstable_id_(1),
last_bloom_skips_(0) {
recoverFromWal();
recoverSstables();
}

void Engine::recoverFromWal() {
    std::ifstream check(wal_path_, std::ios::binary);
    if (!check) {
        return;
    }
    check.close();

    Memtable rebuilt;
    if (!Wal::replay(wal_path_, rebuilt)) {
        return;
    }
    memtable_ = std::move(rebuilt);
}

void Engine::writeSstableManifest() const {
    std::ofstream out(wal_path_ + ".sstables", std::ios::trunc);
    if (!out) {
        return;
    }
    for (const auto& path : sstable_paths_) {
        out << path << '\n';
    }
}

void Engine::recoverSstables() {
    std::ifstream in(wal_path_ + ".sstables");
    if (!in) {
        return;
    }

    std::vector<std::string> paths;
    int max_id = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        paths.push_back(line);

        const std::string prefix = "sstable_";
        const std::string suffix = ".sst";
        if (line.size() > prefix.size() + suffix.size() &&
            line.compare(0, prefix.size(), prefix) == 0 &&
            line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0) {
            std::istringstream num(line.substr(
                prefix.size(), line.size() - prefix.size() - suffix.size()));
            int id = 0;
            if (num >> id && id > max_id) {
                max_id = id;
            }
        }
    }

    sstable_paths_ = std::move(paths);
    if (max_id + 1 > next_sstable_id_) {
        next_sstable_id_ = max_id + 1;
    }
}

bool Engine::gcValueLog() {
    const std::uint32_t old_id = vlog_.segmentId();
    const std::uint32_t new_id = old_id + 1;
    const std::string base = vlog_.basePath();
    const std::string old_path = vlog_.path();

    // Start a clean new file. Wipes leftover bytes if a previous GC crashed.
    ValueLog dest(base, new_id);
    if (!dest.truncate()) {
        return false;
    }

    bool ok = true;
    vlog_.forEachRecord([&](const VlogScanEntry& rec) {
        if (!ok) {
            return;
        }

        // Ask the LSM: is this still the current value for this key?
        const auto payload = lookupPayload(rec.key);
        if (!payload.has_value()) {
            return;  // deleted or never stored
        }
        const auto current = Vptr::decode(*payload);
        if (!current.has_value()) {
            return;
        }
        if (current->segment_id != old_id || current->offset != rec.offset) {
            return;  // key was overwritten; this record is old
        }

        // Still live: copy to the new file and point WAL + memtable at it.
        const auto rewritten = dest.append(rec.key, rec.value);
        if (!rewritten.has_value()) {
            ok = false;
            return;
        }
        const std::string encoded = rewritten->encode();
        if (!wal_.appendPut(rec.key, encoded)) {
            ok = false;
            return;
        }
        memtable_.put(rec.key, encoded);
    });
    if (!ok) {
        return false;
    }

    // Switch to the new file. Crash before this line: we still open the old one.
    if (!ValueLog::storeCurrentSegmentId(base, new_id)) {
        return false;
    }
    vlog_ = std::move(dest);

    if (!flush() || !compact()) {
        return false;
    }

    std::remove(old_path.c_str());
    return true;
}

std::string Engine::makeSstablePath() {
    std::ostringstream oss;
    oss << "sstable_" << next_sstable_id_++ << ".sst";
    return oss.str();
}

bool Engine::flush() {
    if (memtable_.size() == 0) {
        return true;
    }

    const std::string path = makeSstablePath();
    if (!SSTableWriter::write(path, memtable_)) {
        return false;
    }

    sstable_paths_.insert(sstable_paths_.begin(), path);
    memtable_ = Memtable{};
    Wal::truncateFile(wal_path_);
    writeSstableManifest();
    maybeCompact();
    return true;
}

void Engine::maybeFlush() {
    if (memtable_.size() >= flush_threshold_) {
        flush();
    }
}

void Engine::put(const std::string& key, const std::string& value) {
    const auto vptr = vlog_.append(key, value);
    if (!vptr.has_value()) {
        return;
    }
    const std::string payload = vptr->encode();
    wal_.appendPut(key, payload);
    memtable_.put(key, payload);
    maybeFlush();
}

std::optional<std::string> Engine::lookupPayload(const std::string& key) const {
    last_bloom_skips_ = 0;

    if (memtable_.contains(key)) {
        return memtable_.get(key);
    }

    for (const auto& path : sstable_paths_) {
        SSTableReader reader(path);
        if (!reader.mayContain(key)) {
            ++last_bloom_skips_;
            continue;
        }
        if (reader.contains(key)) {
            return reader.get(key);
        }
    }

    return std::nullopt;
}

std::optional<std::string> Engine::resolveValue(const std::string& payload) const {
    const auto vptr = Vptr::decode(payload);
    if (!vptr.has_value()) {
        return std::nullopt;
    }
    return vlog_.read(*vptr);
}

std::optional<std::string> Engine::get(const std::string& key) const {
    const auto payload = lookupPayload(key);
    if (!payload.has_value()) {
        return std::nullopt;
    }
    return resolveValue(*payload);
}

void Engine::remove(const std::string& key) {
    wal_.appendDelete(key);
    memtable_.remove(key);
    maybeFlush();
}

const Memtable& Engine::memtable() const {
    return memtable_;
}

const Wal& Engine::wal() const {
    return wal_;
}

const ValueLog& Engine::vlog() const {
    return vlog_;
}

const std::vector<std::string>& Engine::sstables() const {
    return sstable_paths_;
}

std::size_t Engine::lastBloomSkips() const {
    return last_bloom_skips_;
}

bool Engine::compact() {
    if (sstable_paths_.size() <= 1) {
        return true;
    }
    const std::string output = makeSstablePath();
    const auto old_paths = sstable_paths_;
    if (!SSTableMerger::merge(old_paths, output)) {
        return false;
    }
    for (const auto& path : old_paths) {
        std::remove(path.c_str());
    }
    sstable_paths_ = {output};
    writeSstableManifest();
    return true;
}
void Engine::maybeCompact() {
    if (sstable_paths_.size() > compact_threshold_) {
        compact();
    }
}