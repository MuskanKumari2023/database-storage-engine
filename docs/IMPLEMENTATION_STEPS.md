# Implementation steps

This document describes what was built in each step of the LSM-tree key-value store. Keys and values stay together in the same pipeline (classic LSM, not key-value separation).

```
Phase 0  Binary file I/O
   ↓
Phase 1  Memtable (red-black tree)
   ↓
Phase 2  WAL + crash recovery
   ↓
Phase 3  SSTable flush
   ↓
Phase 4  Unified read path
   ↓
Phase 5  Bloom filter + sparse index
   ↓
Phase 6  Compaction
   ↓
Phase 7  Baseline benchmarks
```

---

## Phase 0 — Binary file I/O

**Goal:** Confirm that bytes can be written to disk and read back unchanged. Later phases (WAL, SSTables) all depend on this.

**What was done**

- Wrote a small round-trip program that opens a file in binary mode, writes a string, closes it, then reads the same file from start to end.
- Compared the bytes that came back with the original string.

This is not part of the engine API. It was a scaffolding check before inventing on-disk record formats.

**Files**

| File | Role |
|---|---|
| `tests/file_io_test.cpp` | Write → read → assert equality |

---

## Phase 1 — Memtable

**Goal:** Keep the latest writes in memory, sorted by key, so a flush can dump them in order.

**What was done**

The memtable is an in-memory key-value map backed by a **red-black tree** (not `std::map`). Each node stores:

- `key`
- `value`
- `type` — `Live` or `Tombstone`
- left / right / parent pointers and a red/black color bit

Operations:

| API | Behavior |
|---|---|
| `put(key, value)` | Insert or overwrite. If the key already exists, the value is updated and the type is set back to `Live`. New keys increment `size_`. |
| `get(key)` | Tree search. Returns the value, or empty if the key is missing or is a tombstone. |
| `contains(key)` | True if the key exists in the tree, including tombstones. Needed so a later delete in the memtable can hide an older SSTable value. |
| `remove(key)` | Logical delete. Existing node becomes a tombstone; if the key is new, a tombstone node is inserted. The key is **not** physically removed. |
| `forEach(fn)` | In-order walk (left → node → right). Calls `fn` for every entry in **sorted key order**. |
| `size()` | Number of keys in the tree (live + tombstone). Used as the flush trigger. |

**Why a red-black tree**

- `put` / `get` are O(log n).
- In-order traversal produces keys already sorted, which SSTable flush requires.
- Inserts rebalance with left/right rotations and the standard uncle/parent color cases so the tree stays balanced.

**Why tombstones exist**

A delete cannot just erase the key. After a flush, an older live value may still sit in an SSTable. The tombstone must travel with the key until compaction can drop it.

**How it was verified**

`tests/memtable_test.cpp`:

- Put / get / overwrite, including a missing key.
- Delete `b`, confirm `get("b")` is empty, confirm `forEach` still visits `b` as a tombstone, then put `b` again and confirm it is live.
- Insert 100 keys and read them all back.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp tests/memtable_test.cpp -o tests/memtable_test
./tests/memtable_test
```

---

## Phase 2 — Write-ahead log and crash recovery

**Goal:** Make writes durable. If the process dies after `put` but before flush, restarting the engine must restore the same memtable.

**What was done**

An append-only WAL file. Every `put` and `remove` is written to disk **before** the memtable is updated.

Record layout (little-endian, records concatenated until EOF):

```
op (1 byte) | key_len (uint32) | val_len (uint32) | key bytes | value bytes
```

- `op = 0x01` — PUT
- `op = 0x02` — DELETE (`val_len = 0`, no value bytes)

`Wal` methods:

| API | Behavior |
|---|---|
| `appendPut` / `appendDelete` | Open the file in append mode, write one record, flush. |
| `replay(path, memtable)` | Read records in order and apply them (`put` or `remove`) onto a memtable. Stops at clean EOF; returns false on a truncated or unknown record. |
| `truncateFile` | Wipe the WAL after a successful flush (added when flush landed in Phase 3). |

The `Engine` constructor calls `recoverFromWal()`. On startup it opens the WAL (if present) and replays into a fresh memtable. That is the crash-recovery path: “process 1” writes, “process 2” is a new `Engine` on the same file.

Write order is fixed: **WAL first, then memtable.** If the process crashes after the WAL append, replay restores the write. If it crashes before the WAL append, the write never happened.

**Files**

| File | Role |
|---|---|
| `wal/wal.h`, `wal/wal.cpp` | Record format, append, replay, truncate |
| `engine/engine.cpp` | `recoverFromWal()` on construct; `put`/`remove` append then update memtable |

**How it was verified**

`tests/wal_test.cpp`:

- Five appends (puts + one delete) and a count of records on disk.
- Replay into a standalone memtable: `user:2` deleted, other keys restored.
- Engine restart: writes in one `Engine` instance, destroy it, open another on the same WAL, assert the same reads.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  engine/engine.cpp tests/wal_test.cpp -o tests/wal_test
./tests/wal_test
```

---

## Phase 3 — SSTables and memtable flush

**Goal:** When the memtable is full, write it to an immutable on-disk sorted table and free memory. The WAL can then be truncated because those writes are now in the SSTable.

**What was done**

`SSTableWriter::write(path, memtable)` walks the memtable with `forEach` and writes entries in key order. Each data entry uses the same byte layout as a WAL record, plus a type byte (`0x01` live, `0x02` tombstone).

After the data section, the file later gained a footer (Phase 5). The data section itself is:

```
[sorted entries] [sparse index] [bloom filter] [20-byte footer]
```

`Engine::flush()`:

1. Skip if the memtable is empty.
2. Write `sstable_N.sst`.
3. Insert that path at the **front** of `sstable_paths_` (newest first).
4. Replace the memtable with an empty one.
5. Truncate the WAL.
6. Ask `maybeCompact()` (no-op until Phase 6 unless the compact threshold is hit).

`maybeFlush()` runs after every `put`/`remove`. If `memtable.size() >= flush_threshold`, it flushes automatically.

`SSTableReader` can look up a key in one file (linear scan first; Phase 5 added index + Bloom).

**Files**

| File | Role |
|---|---|
| `sstable/sstable.h`, `sstable/sstable.cpp` | Writer, reader, later iterator/merger |
| `engine/engine.cpp` | `flush()`, `maybeFlush()`, `makeSstablePath()` |

**How it was verified**

`tests/sstable_test.cpp`:

- Write keys inserted as `c`, `a`, `b` and confirm the file order is `a`, `b`, `c`.
- Reader `get` for hits and a miss.
- Engine with `flush_threshold = 3`: after the third put, memtable size is 0 and one SSTable exists; `engine.get` still returns the three values.
- A memtable tombstone flushed to an SSTable: reader `get` returns empty.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  engine/engine.cpp tests/sstable_test.cpp -o tests/sstable_test
./tests/sstable_test
```

---

## Phase 4 — Unified read path

**Goal:** One `Engine::get` that is correct across memtable plus any number of SSTables.

**What was done**

Reads always go **newest → oldest**:

1. If the memtable `contains` the key, return `memtable.get` (value or empty for a tombstone). Do **not** fall through to disk. That is how a newer overwrite or delete hides older SSTable data.
2. Otherwise walk `sstable_paths_` from newest to oldest.
3. First file that actually contains the key wins.
4. If no file has it, return not found.

This is the LSM “newest-wins” rule:

```
memtable  >  newer SSTable  >  older SSTable
```

Cases covered:

- Key only in the memtable (no flush yet).
- Key only in one SSTable (memtable empty after flush).
- Keys spread across several SSTables.
- Memtable overwrite of a key that was already flushed.
- Memtable tombstone hiding a live value still on disk.

**Files**

| File | Role |
|---|---|
| `engine/engine.cpp` | `get()` newest-wins walk |
| `tests/engine_test.cpp` | Phase 4 cases above |

**How it was verified**

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  engine/engine.cpp tests/engine_test.cpp -o tests/engine_test
./tests/engine_test
```

---

## Phase 5 — Bloom filter and sparse index

**Goal:** Make SSTable reads cheaper. Without this, every miss scans every file from the start.

**What was done**

Two structures are built at flush (and later at compaction) and stored after the data section.

### Sparse index

Every `kIndexInterval`th key (interval = 2) is sampled with its byte offset in the file.

`seekOffsetFor(key)` binary-searches those samples and returns the offset of the **largest indexed key ≤ search key** (or 0). The reader seeks there and scans a short sorted run instead of the whole file.

Stops early when it passes the search key (keys are sorted).

### Bloom filter

A bit array plus several hash functions, sized from `memtable.size()` at flush time (default 1% false-positive rate).

- `add(key)` at write time.
- `mayContain(key)` at read time:
  - **false** — key is definitely not in this SSTable; skip the file.
  - **true** — key *might* be present; then use the sparse index and scan.

False positives are allowed. False negatives are not.

### Footer

20 bytes at EOF:

```
index_offset (uint64) | bloom_offset (uint64) | magic 0x53535442 ("SSTB")
```

On open, the reader seeks to the end, checks the magic, and deserializes the index and Bloom blocks. `getLinearScan` still exists so Phase 7 can compare optimized vs naive reads.

### Engine integration

`Engine::get` calls `mayContain` before opening a file’s data. Each skip increments `lastBloomSkips_`, which the miss benchmark reports.

**Files**

| File | Role |
|---|---|
| `sstable/sparse_index.h`, `sstable/sparse_index.cpp` | Sampled key → offset map |
| `sstable/bloom_filter.h`, `sstable/bloom_filter.cpp` | Membership filter |
| `sstable/sstable.cpp` | Write footer; optimized `get` vs `getLinearScan` |
| `engine/engine.cpp` | Skip SSTables on Bloom miss |

**How it was verified**

`tests/index_test.cpp`:

- Get hits at the start, middle, and end of a 10-key file (index seek).
- `mayContain` is false for a key that was never inserted, true for one that was.
- Engine with two SSTables: a missing key records at least one Bloom skip.
- Phase 4-style get/contains still work after the footer was added.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  engine/engine.cpp tests/index_test.cpp -o tests/index_test
./tests/index_test
```

---

## Phase 6 — Compaction

**Goal:** Bound the number of SSTables. Too many files makes every read check more files, even with Bloom filters.

**What was done**

When `sstable_paths_.size() > compact_threshold`, all SSTables are merged into one new file.

`SSTableMerger::merge(paths, output)`:

1. Open one `SSTableIterator` per input file (iterators only read the data section, not the footer).
2. Paths are **newest first**.
3. Repeatedly pick the smallest current key across iterators (k-way merge).
4. If several files have that key, the **newest** copy wins (lowest index in the newest-first list).
5. If the winner is a tombstone, it is **dropped** (safe because this merge covers the full set of SSTables).
6. Live winners are written to a new SSTable, with a fresh sparse index and Bloom filter.
7. Old files are deleted. `sstable_paths_` becomes the single new path.

`Engine::compact()` can also be called by hand. `maybeCompact()` runs at the end of `flush()`.

**What compaction does not do**

It does not merge the memtable. A key that is only in the memtable stays there. After compact, `get` still checks the memtable first, so an in-memory overwrite still wins.

**Files**

| File | Role |
|---|---|
| `sstable/sstable.cpp` | `SSTableIterator`, `SSTableMerger` |
| `engine/engine.cpp` | `compact()`, `maybeCompact()` |

**How it was verified**

`tests/compaction_test.cpp`:

- Merge two overlapping files: newer `a` wins; unique keys from both sides survive.
- Newer tombstone vs older live value: key is absent from the output.
- Auto-compact: flush threshold 2, compact threshold 3; after a fourth flush the engine has one SSTable and all keys still read correctly.
- Manual compact while `foo` is only in the memtable: `get("foo")` still returns the memtable value.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  engine/engine.cpp tests/compaction_test.cpp -o tests/compaction_test
./tests/compaction_test
```

---

## Phase 7 — Baseline benchmarks

**Goal:** Measure the finished LSM pipeline: write rate, and how much Bloom filters plus the sparse index help reads versus a full linear scan.

**What was done**

A harness (`benchmarks/benchmark.cpp`) and a plot script (`benchmarks/plot_benchmarks.py`). One command builds, runs, and plots:

```bash
bash benchmarks/run_benchmarks.sh
```

Default load: **5000 keys**, **64-byte values**, **flush threshold 100** (~50 SSTables). Compact threshold is set high so compaction does not run during the write bench.

| Benchmark | What it times | Why |
|---|---|---|
| Write throughput | 5000 `put`s | Full path: WAL append + memtable insert + periodic flush |
| Read hit | 2000 gets of a key that exists | Sparse index vs scanning each file from byte 0 |
| Read miss | 2000 gets of a missing key | Bloom filter should skip most SSTables |
| SSTable sweep | Read-hit latency at 1, 2, 4, 8 files | Show how cost grows with file count |

Optimized reads use `Engine::get` (Bloom + sparse index). Linear reads use `getLinearScan` on every SSTable and ignore Bloom filters.

Results go to `benchmarks/output/phase7_baseline.csv` (not committed). Charts are written to `docs/benchmarks/`:

- `phase7_read_comparison.png` — hit/miss, optimized vs linear
- `phase7_sstable_sweep.png` — latency vs SSTable count
- `phase7_write_throughput.png` — puts/sec

Sample numbers from one run (machine-dependent):

| Benchmark | Optimized | Linear scan | Notes |
|---|---|---|---|
| Read hit | ~2077 µs/op | ~7905 µs/op | Index shortens the scan |
| Read miss | ~3724 µs/op | ~17945 µs/op | Bloom skips ~49 of 50 files |
| Write | ~12676 puts/sec | — | WAL + memtable + flush |

With 8 SSTables, optimized hit latency was about **3×** faster than linear scan on that run.

**Files**

| File | Role |
|---|---|
| `benchmarks/benchmark.cpp` | Measurements and CSV |
| `benchmarks/plot_benchmarks.py` | Charts |
| `benchmarks/run_benchmarks.sh` | Build, run, plot |
| `docs/benchmarks/*.png` | Charts embedded in the README |

---

## How the pieces fit together

After all seven steps, a write and a read look like this.

**Write (`put`)**

1. Append a PUT record to the WAL.
2. Insert into the memtable.
3. If the memtable hit `flush_threshold`, write a new SSTable, clear the memtable, truncate the WAL.
4. If SSTable count exceeded `compact_threshold`, merge all SSTables into one.

**Read (`get`)**

1. Memtable first (including tombstones).
2. Each SSTable, newest to oldest: Bloom skip if possible, otherwise sparse-index seek and scan.
3. First real hit wins. If none, not found.

**On restart**

Replay the WAL into a new memtable. Already-flushed data lives in SSTable files; the WAL only holds writes since the last flush.
