# WiscKey progress

Classic LSM (Phases 0–7): [`IMPLEMENTATION_STEPS.md`](IMPLEMENTATION_STEPS.md).

**How this file is updated:** after one task is done, add that task’s section. Do not write later tasks ahead of time. “Up next” is the only preview.

The idea: user values live in a separate file (the vLog). The LSM only stores a small pointer (`vptr`) that says where the value is.

---

## Phase 8 — Value log

### Task 8.1 — Write down the byte layouts first ✅

**Goal:** Decide the on-disk format, and the order of writes, before coding.

**What was done**

The spec is the comment at the top of `vlog/vlog.h`.

The LSM no longer stores `"hello"`. It stores a 17-byte **vptr**:

```
tag (1 byte) | which file (4) | where in the file (8) | how long the value is (4)
```

- `tag = 0x02` means “this is a pointer into the vLog”.
- Phase 8 uses one file, so “which file” is always `1`.

Each vLog record is:

```
key length | value length | key | value
```

The key is stored so GC can later ask “do we still need this value?”

Write order for `put`:

```
1. Append the value to the vLog. Get a vptr back.
2. Write that vptr to the WAL.
3. Put the same vptr in the memtable.
```

If we crash after step 1, the vLog has an unused value. If we crash after step 2, replay restores the vptr.

**Why this order:** the WAL should stay small. It stores a pointer, not the full value.

---

### Task 8.2 — Write values into the vLog ✅

**Goal:** `put` appends the value to a file, then the LSM stores only the pointer.

**What was done**

- `ValueLog::append` writes one record and returns `{file, offset, size}`.
- `Vptr::encode` / `decode` turn that into 17 bytes and back.
- `Engine::put` writes the vLog first, then WAL + memtable. If the vLog write fails, nothing is written to the WAL.
- Flush still works the same way. SSTables now contain vptrs, not user values.
- File name: `test.wal` → `test.vlog`.

**Why:** the value is written once, in order. Compaction copies keys and 17-byte pointers, not the big values.

**How verified:** `tests/vlog_test.cpp` — three records appear in the `.vlog` in order. After a flush, SSTable `get("k1")` is a vptr, not `"alpha"`.

---

### Task 8.3 — Read the value back in `get` ✅

**Goal:** `get` still returns the user value. Phase 4 tests still pass.

**What was done**

`get` is two steps:

1. `lookupPayload(key)` — find the vptr (memtable first, then SSTables newest to oldest).
2. `ValueLog::read` — jump to that offset and return the value.

If the key was deleted or is missing, step 1 returns nothing and we never open the vLog.

Opening a new `Engine` on the same WAL + vLog still returns the values.

**Why two functions:** the LSM stores pointers. The user still wants values.

**How verified:** crash/reopen test in `tests/vlog_test.cpp`; `tests/engine_test.cpp` still passes.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  vlog/vlog.cpp engine/engine.cpp tests/vlog_test.cpp -o tests/vlog_test
./tests/vlog_test
```

---

## Checkpoint 5

The vLog only grows. Overwrites and deletes leave old values on disk. Nothing cleans them up yet.

---

## Phase 9 — Cleaning up the vLog (GC)

Overwrites and deletes leave unused records in the vLog. GC copies the values we still need into a new file and deletes the old one.

### Task 9.1 — Name the files ✅

**Goal:** Write down which files GC uses, before coding.

**What was done**

Added to `vlog/vlog.h`. Example for `test.vlog`:

| File | Meaning |
|---|---|
| `test.vlog` | First file (segment 1). This is what we read during the first GC. |
| `test.vlog.2` | New file (segment 2). Live values are copied here. New puts go here after GC. |
| `test.vlog.current` | One number: which file is active. If this file is missing, use 1. |

We GC the whole file (not a slice of it).

**Why:** we finish writing the new file first. Only then do we update `.current`. If GC crashes, `.current` still says 1, so we ignore a half-written `test.vlog.2`.

---

### Task 9.2 — Keep live values, drop old ones ✅

**Goal:** One GC pass copies records we still need and skips the rest.

**What was done**

`Engine::gcValueLog()`:

1. Create an empty new file (`test.vlog.2`).
2. Read every record in the old file. Look up that key in the LSM.
   - If the LSM still points at **this exact record** (same file and offset), copy it to the new file. Write the new vptr to the WAL and memtable.
   - If the key was overwritten or deleted, skip it.
3. Do **not** call `Engine::put` here. `put` would append the value a second time. We append to the new file ourselves, then only update WAL + memtable.

Example: 10 keys, then overwrite 5 of them → 15 records on disk. After GC the new file has **10** records (the 5 overwritten keys still exist; they just have a newer value). A deleted key is gone.

**Why we match file + offset, not just “key exists”:** the key might exist with a *newer* value written later in the same file. The old record is unused.

---

### Task 9.3 — Crash in the middle of GC ✅

**Goal:** Crash before `.current` is updated → keep using the old file. Crash after → use the new file, then we can delete the old one.

**What was done**

- `ValueLog::read` can open any file, not only the current one. Old vptrs still work until we delete that file.
- If a previous GC left a junk `test.vlog.2`, we empty it before writing.
- We write `.current` only after the new file is complete. Then we switch to it, flush, compact, and delete the old file.
- After flush the WAL is empty, so we also save the SSTable file list (`test.wal.sstables`) and reload it on restart.

**How verified**

- Write garbage into `test.vlog.2`, restart: old values still come back (we never updated `.current`).
- Run a full GC, restart: values come from `test.vlog.2`, and `test.vlog` is gone.

```bash
g++ -std=c++17 -Wall -Wextra -I. memtable/memtable.cpp wal/wal.cpp \
  sstable/bloom_filter.cpp sstable/sparse_index.cpp sstable/sstable.cpp \
  vlog/vlog.cpp engine/engine.cpp tests/vlog_test.cpp -o tests/vlog_test
./tests/vlog_test
```

---

## Checkpoint 6

GC copies live values into a new file and drops unused ones. `.current` is what makes the new file official. The old file is deleted only after flush and compact.

---

## Up next

**Task 10.1** — Benchmarks: write and compaction time with the vLog vs without it.
