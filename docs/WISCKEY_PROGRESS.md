# WiscKey progress

Classic LSM (Phases 0–7): [`IMPLEMENTATION_STEPS.md`](IMPLEMENTATION_STEPS.md).

**How this file is updated:** after **one task** lands, add that task’s section (goal, what was done, why, how verified). Do not write later tasks in advance. The “Up next” line is the only preview.

---

## Phase 8 — Value log

### Task 8.1 — Formats as a spec (before code) ✅

**Goal:** Written byte layouts for the vptr and the vLog record, plus a decision on WAL vs vLog order (roadmap issue #4).

**What was done**

Spec lives in the comment at the top of `vlog/vlog.h`.

vptr (17 bytes, little-endian) — LSM / WAL / SSTable payload:

```
tag (1) | segment_id (uint32) | offset (uint64) | value_size (uint32)
```

- `tag = 0x02`. `0x01` reserved for a later inline-small-value path.
- `offset` = start of the vLog record. `value_size` = user value length.
- Phase 8: one file, `segment_id = 1`.

vLog record (append-only):

```
key_len (uint32) | val_len (uint32) | key bytes | value bytes
```

Key is stored so a later GC pass can check the LSM. No CRC in v1.

Write order:

```
1. vLog.append(k, v) → vptr
2. WAL.appendPut(k, encode(vptr))
3. memtable.put(k, encode(vptr))
```

Crash after 1: orphan in the vLog. Crash after 2: replay restores the vptr.

**Why:** if the WAL stored the raw value, it would grow with large values and replay would re-append to the vLog. Pointer-in-WAL keeps the log small.

**Done when:** spec exists in `vlog/vlog.h` before the `.cpp` logic.

---

### Task 8.2 — Value-log writer ✅

**Goal:** On `put`, append the value sequentially, get a vptr, store that vptr in the memtable/WAL/SSTable path.

**What was done**

- `ValueLog::append` writes one record, flushes, returns `{segment_id, offset, value_size}`.
- `Vptr::encode` / `decode` for the 17-byte payload.
- `Engine::put`: vLog first, then WAL + memtable with `vptr.encode()`. Failed vLog append does not write a WAL pointer.
- Flush is unchanged: SSTables now contain vptrs. Path: `test.wal` → `test.vlog`.

**Why:** values are written once, in order. Compaction still only copies keys + 17-byte pointers.

**How verified:** `tests/vlog_test.cpp` — three appends parsed from the `.vlog` in order; after a flush, SSTable `get("k1")` is a vptr, not `"alpha"`.

---

### Task 8.3 — Value-log reader in `get` ✅

**Goal:** Phase 4 tests still pass, going through find-vptr then resolve-vptr.

**What was done**

- `lookupPayload(key)` = old LSM walk (memtable, then SSTables newest → oldest).
- `get(key)` decodes the payload as a vptr and calls `ValueLog::read` (seek to `offset`, return the value).
- Tombstone / missing: `lookupPayload` empty → no vLog read.
- Reopen `Engine` on the same WAL + vLog restores values.

**Why split `lookupPayload` / `get`:** LSM still stores pointers; the user API still returns values.

**How verified:** `tests/vlog_test.cpp` crash reopen; `tests/engine_test.cpp` (Phase 4) still passes.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  vlog/vlog.cpp engine/engine.cpp tests/vlog_test.cpp -o tests/vlog_test
./tests/vlog_test
```

---

## Checkpoint 5

Value log only grows. Overwrites and deletes leave stale records. No GC yet.

---

## Up next

**Task 9.1** — Head/tail spec for vLog GC (written spec only; no code until that lands).
