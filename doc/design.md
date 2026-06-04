# Design — Key-Value DB on `flash_area`

Status: v1 · Target board for v1: `native_sim`

---

## 1. Scope

This project delivers **one Zephyr library module**: a key-value database under `lib/kv_db`, built directly on Zephyr's `flash_area` API (`<zephyr/storage/flash_map.h>`).

**UBI is out of scope for this project.** A UBI-like flash-translation layer is being designed separately. Because UBI's upper interface will be `flash_area`-shaped, this KV DB can be moved on top of UBI later **without code changes** — only the partition it opens would change. KV therefore uses **only** the public `flash_area_*` API and no flash-driver internals.

### Target partition

`storage_partition` (label `"storage"`) is the partition KV opens via `FIXED_PARTITION_ID(storage_partition)`. The library is partition-agnostic — sizing comes entirely from the device tree.

### Capacity

The target DB capacity is **8 MB**. With an estimated average entry footprint of ~80 B (6 B header + ~64 B avg key + ~8 B avg value + 2 B CRC), this comfortably accommodates the **100 000-entry structural target** from §1.4.

`native_sim`'s stock storage partition is only 16 KB at 0xfc000 in a 2 MB simulated flash — insufficient for verifying at scale. The repo provides a device-tree overlay (`app/boards/native_sim.overlay`) that:

- Extends the simulated flash to 16 MB.
- Resizes `storage_partition` to 8 MB at offset 0x800000 (after the default mcuboot / slot partitions, which are left in place).

On real hardware, the platform DT declares an 8 MB partition directly; no overlay needed.

### Goals

- String-key → byte-blob store with persistence across reboots.
- Self-validating on-flash format (CRC per entry; CRC per sector header).
- Crash-safe: torn writes don't corrupt prior data.
- Buildable, runnable, and ztest-able on `native_sim` (use `--flash=<file>` for cross-process persistence).
- Portable: only uses standard `flash_area_*` API.

### Non-goals (v1)

Multi-thread safety, wear leveling beyond simple half-rotation, encryption, streaming/large-value support, iteration with mutation, multi-instance DBs.

### Design constraints (load-bearing)

These constraints shape the algorithms and disqualify the obvious "RAM index" approach used by typical embedded K-V libraries:

- **Minimum steady-state RAM.** Total RAM owned by `kv_db` between calls is a small handle: partition pointer, active-half index, current generation, write cursor — on the order of ~32 bytes. **No per-entry RAM.**
- **No caching.** Keys, values, and offsets are not held in RAM between operations. Every `get` re-reads from flash. Every `count` rescans. No memoization, no Bloom filter, no hash table.
- **Flash reads are fast.** Cost model: a flash read is cheap; a flash write/erase is expensive. Algorithms favor read-heavy designs over write-heavy ones.
- **Structurally ready for 100 000 entries in 8 MB.** No fixed-size array sized to expected entry count. No O(n) RAM growth. The natural ceiling on entry count is partition size, not a Kconfig limit. 8 MB / ~80 B per entry ≈ 100 k entries.

### Cost summary (per operation)

| Operation | Flash reads | Flash writes | RAM |
|---|---|---|---|
| `mount`         | O(n) header-only scan (6 B per entry) to find write cursor | 0 (or 1 if formatting empty partition) | O(1) |
| `set`           | 0 (latest-wins on next scan, no dedup check) | 1 entry append (+ compact path on full log) | O(1) |
| `get`           | O(n) over active half | 0 | O(1) (one entry-header buffer) |
| `delete`        | O(n) to locate latest occurrence | 0 or 1 (tombstone, only if key currently live) | O(1) |
| `count`         | O(n²) (forward dedupe scan) | 0 | O(1) |
| `iterate`       | O(n²) (per-entry latest-check) | 0 | O(1) |
| `compact`       | O(n²) | O(live) | O(1) |
| `clear`         | O(1) | 1 sector header + erases | O(1) |

`n` is the number of entries currently on flash in the active half. Compaction reclaims tombstones and overridden entries, keeping `n` bounded by what fits in a half.

The O(n²) operations are an explicit consequence of the "no RAM index, no caching" constraint. v2 can offer Kconfig opt-ins for transient-RAM dedup buffers if profiling demands it.

---

## 2. Architecture

```
+------------------------------------------+
|  app/src/main.c                  (demo)  |
+------------------------------------------+
|  kv_db   (lib/kv_db)                     |    string key → byte blob
|    - log-structured store                |
|    - 2-half compaction                   |
|    - O(1) steady-state RAM               |
+------------------------------------------+
|  flash_area                  (Zephyr)    |    storage_partition
+------------------------------------------+
|  sim-flash                 (native_sim)  |
+------------------------------------------+
```

Only the boundary marked `flash_area` is touched by KV. A future UBI layer can sit between `flash_area` and `sim-flash` without affecting KV code.

---

## 3. On-flash format

The partition is split at runtime into two **halves** of equal size, each spanning ≥ 1 erase sector. At any moment, exactly one half is **active** (accepts appends) and one is **scratch** (used by compaction). Atomicity is provided by per-entry CRCs and per-half sealed generation counters.

### 3.1 Half layout

```
Half (e.g. 2 sectors = 8 KB on native_sim):

  offset 0x000   Sector header (16 B)
  offset 0x010   Entry stream
  offset ...     (rest erased = 0xff)
```

### 3.2 Sector header (16 B, `__packed`)

```
magic[4]     = 'K','V','D','H'
gen[4]       = monotonic generation counter for this half (LE)
version[2]   = 1
reserved[2]  = 0
hdr_crc32[4] = CRC32-IEEE over preceding 12 B
```

Written exactly once per erase cycle of the half, immediately after erase. A half whose sector header magic+CRC are invalid is "unformatted".

### 3.3 Entry (variable length, written sequentially)

```
struct kv_entry_hdr {                /* 6 B, __packed */
    uint8_t  magic[2];               /* 'K','V' */
    uint8_t  flags;                  /* bit0 TOMBSTONE */
    uint8_t  key_len;                /* 1..MAX_KEY_LEN */
    uint16_t val_len;                /* 0..MAX_VAL_LEN (LE) */
};
/* followed by: */
/*   key[key_len]                       (no NUL terminator)    */
/*   val[val_len]                       (omitted if TOMBSTONE) */
/*   uint16_t entry_crc16  (LE)         CRC16-CCITT over hdr+key+val */
```

A `TOMBSTONE` entry has `val_len = 0` and represents "key is now deleted". The latest entry (tombstone or normal) wins on any scan.

### 3.4 End-of-log marker

There isn't one. The end of the log is the first offset where any of the following holds:

- header magic ≠ `'K','V'`
- `key_len == 0` or `key_len > MAX_KEY_LEN`
- `val_len > MAX_VAL_LEN`
- entry would exceed half boundary
- entry CRC fails

The cursor is set to that offset. Erased flash (0xff…) naturally fails the magic test.

### 3.5 Why "latest wins on scan"

Because there is no RAM index, every read of a key must scan and pick the **last** matching entry in append order. This makes every `set` an O(1) operation (no dedup check needed) and naturally handles overwrites and deletes via tombstones — at the cost of read-time work, which the constraints accept.

---

## 4. Algorithms

All algorithms maintain only the steady-state RAM defined in §1.4: partition handle, active half index, current generation, write cursor. No per-entry allocations.

### 4.1 Mount

```
fa = flash_area_open(partition)
size = flash_area_get_size(fa)
half_size = size / 2 (aligned down to erase block)

read sector header of half 0 → valid_0, gen_0
read sector header of half 1 → valid_1, gen_1

if (!valid_0 && !valid_1):
    erase half 0
    write sector header with gen=1 on half 0
    active = 0
elif (valid_0 && !valid_1): active = 0
elif (!valid_0 && valid_1): active = 1
else:                       active = (gen_0 > gen_1) ? 0 : 1
    # the other half is leftover from a torn compaction; will be re-used on next compact

# Find write cursor by reading entry HEADERS ONLY (6 bytes each), skipping bodies:
cursor = 0x10
while cursor + sizeof(entry_hdr) ≤ half_size:
    read 6-byte header at cursor
    if magic invalid OR key_len out of range OR val_len out of range OR
       entry would overrun half: break
    cursor += 6 + key_len + val_len + 2   # skip body + crc; do NOT verify CRC here
write_cursor = cursor
```

Mount cost: O(n) flash reads of 6 bytes each. For 100k entries that is 600 KB. CRCs of entries are validated lazily on `get` (the scan rejects bad entries by stopping). Headers are cheap; verifying every CRC at mount would multiply the cost by ~20× with no benefit since the same CRC is checked when the entry is read.

### 4.2 `kv_db_set(key, val, len)`

```
entry_size = 6 + key_len + val_len + 2
if write_cursor + entry_size > half_end:
    compact()                  # may also fail with -ENOSPC if compacted size still > half
    if still no room: return -ENOSPC

build entry in a small stack buffer (header + key + val + crc)
flash_area_write(active half, write_cursor, buf, entry_size)
write_cursor += entry_size
```

No duplicate check. A later `get` finds the latest write by scan order.

### 4.3 `kv_db_delete(key)`

```
# Locate the latest occurrence of this key in the active half.
cursor = 0x10
latest_flags = NONE
while cursor < write_cursor:
    read header
    if key_len matches:
        flash_area_read(cursor + 6, scratch, key_len)
        if scratch matches key:
            latest_flags = header.flags
    cursor += entry_size

if latest_flags == NONE: return -ENOENT
if latest_flags & TOMBSTONE: return -ENOENT   # already deleted

append tombstone entry  (val_len = 0, flags = TOMBSTONE)
```

O(n) flash reads, 0 or 1 flash writes. Strict semantics: deleting a missing or already-deleted key returns `-ENOENT` and writes nothing — repeated deletes of a nonexistent key cannot fill the log.

### 4.4 `kv_db_get(key, out, out_size, *out_len)`

```
cursor = 0x10
latest_value_off = -1, latest_len = 0, latest_tombstone = false
key_buf[MAX_KEY_LEN]

while cursor < write_cursor:
    read header
    if header.key_len == strlen(key):
        flash_area_read(cursor + 6, key_buf, header.key_len)
        if memcmp(key_buf, key, header.key_len) == 0:
            latest_value_off = cursor + 6 + header.key_len
            latest_len = header.val_len
            latest_tombstone = header.flags & TOMBSTONE
    cursor += entry_size

if latest_value_off < 0 or latest_tombstone: return -ENOENT
if out_size < latest_len: return -ENOMEM
flash_area_read(fa, latest_value_off, out, latest_len)
# CRC is verified by reading the trailing crc16 and comparing to one we compute
# while we just read the entry body.
*out_len = latest_len
```

O(n) flash reads. RAM = one key buffer (`MAX_KEY_LEN` bytes) on the stack.

### 4.5 `kv_db_compact()`

The compaction goal: produce a new half containing exactly one entry per live key (no tombstones, no overridden entries), then atomically swap. To do this without an in-RAM dedup table, compaction uses an **O(n²) live-check**:

```
other = active ^ 1
flash_area_erase(other)
# leave sector header offset (0x00..0x10) as 0xff for now

write_cursor_other = 0x10
cursor = 0x10
while cursor < write_cursor:
    read header at cursor
    if header.flags & TOMBSTONE: skip          # tombstones never survive compaction
    else if has_later_occurrence(active, cursor, key):
        skip   # an entry farther forward overrides this one (or tombstones it)
    else:
        copy entry to other half at write_cursor_other
        write_cursor_other += entry_size
    cursor += entry_size

# Seal point — one write commits the new half:
write sector header on 'other' with gen = active.gen + 1, valid CRC

flash_area_erase(active)
active = other
write_cursor = write_cursor_other
```

`has_later_occurrence(half, start, key)` performs a forward scan from `start + entry_size` to `write_cursor` and returns true if any later entry has the same key (whether normal or tombstone). This is the O(n²) factor.

Sealing the sector header **last** is the atomicity trick: until the seal write completes, the scratch half is "unformatted" from mount's perspective, so a crash falls back to the old half. Crash analysis:

| Crash point | Outcome |
|---|---|
| During entry copy on scratch | Scratch header still 0xff → mount picks old half. ✓ |
| Mid-sector-header write | Header CRC fails → mount picks old half. ✓ |
| After scratch sealed, before old erased | Both valid; mount picks higher `gen` (= scratch). ✓ |
| During old erase | Same: scratch wins. ✓ |

### 4.6 `kv_db_count()` / `kv_db_iterate(cb, user)`

Both share the same O(n²) shape as compaction: forward scan, with a "later occurrence" probe to decide whether the current entry is the *latest* for its key. `iterate` invokes the callback only for non-tombstone entries that are the latest. `count` increments a counter for the same set.

### 4.7 `kv_db_clear()`

Erase both halves; write sector header on half 0 with `gen = max(prev_gen) + 1`; reset write cursor to 0x10. Idempotent. O(1) flash writes after the erases.

---

## 5. Steady-state RAM

Total RAM owned by `kv_db` between calls:

```c
struct kv_db_state {
    const struct flash_area *fa;       /* 8 B (pointer) */
    uint32_t                 fa_size;  /* 4 B (cached partition size) */
    uint32_t                 half_size;
    uint32_t                 write_cursor;
    uint32_t                 gen;
    uint8_t                  active;   /* 0 or 1 */
    bool                     mounted;
};
```

≈ 32 bytes. **No per-entry data structures.** No caches, no Bloom filters, no key tables.

Per-call stack usage during `get` / `delete` / `iterate` peaks at one `MAX_KEY_LEN` buffer (default 64 B) plus the entry header (6 B) — well under 100 B.

`set` additionally needs a small write buffer; it can either reuse caller memory (build the entry directly via three sequential `flash_area_write` calls — header, key+val, crc) or allocate `entry_size` on the stack (bounded by `6 + MAX_KEY_LEN + MAX_VAL_LEN + 2`, default ~330 B). Implementation will use sequential writes to keep stack usage O(1) independent of entry size.

---

## 6. Public API (`include/app/lib/kv_db.h`)

```c
int    kv_db_mount(void);
int    kv_db_unmount(void);

int    kv_db_set(const char *key, const void *val, size_t len);
int    kv_db_get(const char *key, void *out, size_t out_size, size_t *out_len);
int    kv_db_delete(const char *key);

int    kv_db_clear(void);
int    kv_db_compact(void);
size_t kv_db_count(void);

/* introspection (tests / debug) */
int    kv_db_iterate(int (*cb)(const char *key, size_t key_len,
                                const void *val, size_t val_len,
                                void *user),
                     void *user);
```

Errors: `-ENOENT`, `-ENOSPC`, `-ENOMEM`, `-EINVAL`, `-EIO`, `-ENODEV` (not mounted).

**Concurrency contract (v1):** single-threaded. Caller serializes. Documented in the header.

**Cost note in header:** `get`, `delete`, `count`, `iterate`, `compact` document their O(n) / O(n²) flash-read cost so callers don't expect index-backed performance.

---

## 7. Kconfig

`lib/kv_db/Kconfig` (sourced from existing `lib/Kconfig` menu):

```
KV_DB                    bool   "Key-value database library"
                         select FLASH
                         select FLASH_MAP
                         select CRC
KV_DB_PARTITION_LABEL    string default "storage"
KV_DB_MAX_KEY_LEN        int    default 64    range 1 255
KV_DB_MAX_VAL_LEN        int    default 256   range 1 4096
module = KV_DB / module-str = KV_DB    /* standard LOG module pattern */
```

**No `KV_DB_INDEX_CAPACITY`** — there is no in-RAM index to size. Entry count is bounded only by partition size.

Half size is derived at runtime: `flash_area_get_size() / 2`, aligned down to erase block size.

---

## 8. Repo integration

Mirrors the existing `lib/custom/` pattern.

```
lib/
  CMakeLists.txt           add_subdirectory_ifdef(CONFIG_KV_DB kv_db)
  Kconfig                  rsource "kv_db/Kconfig"   (inside existing menu)

  kv_db/
    CMakeLists.txt
    Kconfig
    kv_db.c                public API + compaction
    kv_db_internal.h       on-flash structs, BUILD_ASSERTs

include/app/lib/
  kv_db.h

tests/lib/kv_db/
  CMakeLists.txt
  prj.conf
  testcase.yaml
  src/main.c               ztest

app/
  boards/native_sim.overlay  extend sim-flash to 16 MB; resize storage_partition to 8 MB at 0x800000
  prj.conf                   CONFIG_KV_DB=y, CONFIG_FLASH=y, CONFIG_FLASH_MAP=y, CONFIG_CRC=y, CONFIG_LOG=y
  src/main.c                 demo: mount, RMW "boot_count" u32, log
```

---

## 9. Zephyr APIs used

- `<zephyr/storage/flash_map.h>` — `FIXED_PARTITION_ID`, `flash_area_open`, `flash_area_close`, `flash_area_read`, `flash_area_write`, `flash_area_erase`, `flash_area_get_size`, `flash_area_get_sectors`
- `<zephyr/sys/crc.h>` — `crc16_ccitt(0xffff, …)` for entries; `crc32_ieee(…)` for sector headers
- `<zephyr/logging/log.h>` — module logger

---

## 10. Failure-mode summary

| Scenario | Outcome |
|---|---|
| Crash mid-entry-write | Entry CRC fails on next scan → log truncated at that point; previous values intact. |
| Crash before scratch sector header sealed during compact | Scratch has invalid header → mount picks old half. |
| Crash after scratch sealed but before old erased | Both valid; mount picks higher `gen` (= scratch). Old half re-used by next compaction. |
| Crash during old-half erase | Same as above. |
| Bit corruption in entry value | CRC catches it on read; entry treated as end-of-log by mount. *Anything after that entry is also lost.* (v2 could keep scanning past one bad entry; learning-grade v1 truncates.) |
| Partition full (compacted size still > half) | `kv_db_set` returns `-ENOSPC`; caller deletes a key then retries. |

---

## 11. Testing strategy

`tests/lib/kv_db/` ztest, run via `west twister -p native_sim -T tests/lib/kv_db`.

Cases:

1. `mount_empty_formats_partition` — fresh erase, mount OK, `count()==0`
2. `set_get_roundtrip` — single key, binary value with NUL bytes inside
3. `get_missing_returns_enoent`
4. `overwrite_returns_latest` — set k=A, set k=B, get → B; `count()` unchanged
5. `delete_then_get_enoent`; `delete_missing_returns_enoent`; `double_delete_returns_enoent_second_time`
6. `persistence_across_remount` — set N keys, unmount, mount, all present
7. `boundary_key_len`, `boundary_val_len` — at MAX, MAX+1 (reject), 1, 0 (reject empty key; allow empty value)
8. `crc_corruption_detected` — manually corrupt a byte via `flash_area_write`, re-mount, log truncated cleanly
9. `log_full_triggers_compaction` — fill log; next set causes compaction; data preserved
10. `compaction_drops_tombstones` — set k1, set k2, delete k1, compact, scan flash → only k2 present
11. `mount_picks_higher_generation` — manually seed both halves with different gens, verify selection
12. `clear_resets_db` — populate, clear, mount → empty
13. `iterate_visits_each_live_key_once` — callback count matches `count()`
14. `scale_smoke` — fill the active half until the next set triggers compaction; verify get/delete/compact still work. With the 8 MB overlay in place, the suite can also exercise a heavier `scale_100k` variant (gated by a Kconfig flag) that inserts ~100 000 small entries and verifies a representative subset round-trips. Without the overlay, this case falls back to ~80 entries on the stock 16 KB partition.

For cross-process persistence tests, `testcase.yaml` declares `extra_args: --flash=/tmp/kv_db_test.bin`. Most cases use in-process unmount/mount to exercise the same persistence paths without needing the flash file across runs.

---

## 12. Verification (end-to-end)

1. `west build -b native_sim app -p` — clean build, no warnings. The overlay at `app/boards/native_sim.overlay` is auto-applied by Zephyr's build system.
2. `./build/zephyr/zephyr.exe --flash=/tmp/kv.bin --stop_at=2` — first run prints `boot_count=1`, sets `hello → "world"`. Second run prints `boot_count=2`, reads `hello` back. Delete `/tmp/kv.bin` → resets. The flash file should be ~16 MB on disk (matches the resized sim-flash).
3. `west twister -p native_sim -T tests/lib/kv_db` — all cases green.
4. `ls -l /tmp/kv.bin` confirms 16 MB; `xxd /tmp/kv.bin | grep -c KVDH` ≥ 1; `grep -c KV` — many entry-header matches.

---

## 13. Forward-compat note (UBI)

UBI is a separate, future module that will present a `flash_area`-shaped interface. When that exists, KV gets pointed at UBI's partition instead of the platform's `storage_partition` (via Kconfig `KV_DB_PARTITION_LABEL`). No KV source change needed.

---

## 14. Critical files (relative to repo root)

- `lib/kv_db/kv_db.c` — implementation
- `lib/kv_db/kv_db_internal.h` — on-flash structs
- `lib/kv_db/Kconfig` — options
- `lib/kv_db/CMakeLists.txt`
- `include/app/lib/kv_db.h` — public API
- `lib/CMakeLists.txt`, `lib/Kconfig` — registration
- `app/prj.conf`, `app/src/main.c` — enable + demo
- `tests/lib/kv_db/` — ztest suite
