# Design — Key-Value DB on `flash_area`

Status: v1 · Target board for v1: `native_sim`

---

## 1. Scope

This project delivers **one Zephyr library module**: a key-value database under `lib/kv_db`, built directly on Zephyr's `flash_area` API (`<zephyr/storage/flash_map.h>`).

**UBI is out of scope for this project.** A UBI-like flash-translation layer is being designed separately. Because UBI's upper interface will be `flash_area`-shaped, this KV DB can be moved on top of UBI later **without code changes** — only the partition it opens would change. KV therefore uses **only** the public `flash_area_*` API and no flash-driver internals.

### Target partition

`storage_partition` (label `"storage"`) is already declared in `native_sim.dts`: 16 KB at 0xfc000 in `&flash0`, four 4 KB sectors, 1-byte write granularity, 0xff erase value. Accessible as `FIXED_PARTITION_ID(storage_partition)`. **No devicetree overlay required.**

### Goals

- String-key → byte-blob store with persistence across reboots.
- Self-validating on-flash format (CRC per entry; CRC per sector header).
- Crash-safe: torn writes don't corrupt prior data.
- Buildable, runnable, and ztest-able on `native_sim` (use `--flash=<file>` for cross-process persistence).
- Portable: only uses standard `flash_area_*` API.

### Non-goals (v1)

Multi-thread safety, wear leveling beyond simple half-rotation, encryption, streaming/large-value support, iteration with mutation, dynamic index resizing, multi-instance DBs.

---

## 2. Architecture

```
+------------------------------------------+
|  app/src/main.c                  (demo)  |
+------------------------------------------+
|  kv_db   (lib/kv_db)                     |    string key → byte blob
|    - log-structured store                |
|    - 2-half compaction                   |
|    - in-RAM index                        |
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

A `TOMBSTONE` entry has `val_len = 0` and removes the key from the RAM index.

### 3.4 End-of-log marker

There isn't one. The end of the log is the first offset where any of the following holds:

- header magic ≠ `'K','V'`
- `key_len == 0` or `key_len > MAX_KEY_LEN`
- `val_len > MAX_VAL_LEN`
- entry would exceed half boundary
- entry CRC fails

The cursor is set to that offset. Erased flash (0xff…) naturally fails the magic test.

---

## 4. Algorithms

### 4.1 Mount

```
fa = flash_area_open(storage_partition)
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
else: active = (gen_0 > gen_1) ? 0 : 1
    # the other half is leftover from a torn compaction; will be re-used on next compact

scan active half:
    cursor = 0x10
    while cursor + sizeof(hdr) ≤ half_size:
        read hdr; validate magic/lengths; if bad → break
        read key, val, crc; if crc bad → break
        if TOMBSTONE: index_remove(key)
        else:         index_upsert(key, flash_offset_of_value, val_len)
        cursor += entry_size
    write_cursor = cursor
```

### 4.2 `kv_db_set(key, val, len)`

```
if size of new entry > remaining space in active half: compact()
if still no room: return -ENOSPC

write entry at write_cursor on active half
index_upsert(key, value_offset, len)
write_cursor += entry_size
```

### 4.3 `kv_db_delete(key)`

```
if key not in index: return -ENOENT
if size of tombstone > remaining space: compact()
write tombstone entry
index_remove(key)
write_cursor += entry_size
```

### 4.4 `kv_db_get(key, out, out_size, *out_len)`

```
e = index_lookup(key)
if !e: return -ENOENT
if out_size < e.val_len: return -ENOMEM
flash_area_read(fa, e.value_offset, out, e.val_len)
*out_len = e.val_len
```

### 4.5 `kv_db_compact()`

The sector header is written **last** so that a partial compaction is invisible to mount: the scratch half stays "unformatted" (header still 0xff) until every live entry has been copied.

```
other = active ^ 1
flash_area_erase(other)
# leave sector header offset (0x00..0x10) as 0xff for now

write_cursor_other = 0x10
for each live entry in RAM index:
    read value from active half
    write entry to other half at write_cursor_other
    update index pointer to point into 'other'
    write_cursor_other += entry_size

# Seal point — one write commits the new half:
write sector header on 'other' with gen = active.gen + 1, valid CRC

flash_area_erase(active)
active = other
write_cursor = write_cursor_other
```

Crash analysis:

| Crash point | Outcome |
|---|---|
| During entry copy on scratch | Scratch header still 0xff → mount picks old half. ✓ |
| Mid-sector-header write | Header CRC fails → mount picks old half. ✓ |
| After scratch sealed, before old erased | Both valid; mount picks higher `gen` (= scratch). ✓ |
| During old erase | Same: scratch wins. ✓ |

### 4.6 `kv_db_clear()`

Erase both halves; write sector header on half 0 with `gen = max(prev_gen)+1`; reset RAM index. Idempotent.

---

## 5. In-RAM index

Fixed-capacity array (size from Kconfig). Linear scan for v1 (default capacity 32 → trivial cost).

```c
struct kv_index_entry {
    bool     used;
    uint8_t  key_len;                            /* without NUL */
    uint16_t val_len;
    uint32_t value_offset;                       /* absolute offset within partition */
    char     key[CONFIG_KV_DB_MAX_KEY_LEN + 1];  /* NUL-terminated */
};
static struct kv_index_entry index[CONFIG_KV_DB_INDEX_CAPACITY];
```

On `set` to a duplicate key, the existing slot is reused (new offset/len replaces old). On `delete`, the slot is freed (`used = false`). `index_full` → `kv_db_set` returns `-ENOSPC`.

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
KV_DB_INDEX_CAPACITY     int    default 32    range 1 1024
module = KV_DB / module-str = KV_DB    /* standard LOG module pattern */
```

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
  prj.conf                 CONFIG_KV_DB=y, CONFIG_FLASH=y, CONFIG_FLASH_MAP=y, CONFIG_CRC=y, CONFIG_LOG=y
  src/main.c               demo: mount, RMW "boot_count" u32, log
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
| Bit corruption in entry value | CRC catches it; entry treated as end-of-log. *Anything after that entry is also lost.* (v2 could keep scanning past one bad entry; learning-grade v1 truncates.) |
| Index full | `kv_db_set` returns `-ENOSPC`; caller deletes a key then retries. |

---

## 11. Testing strategy

`tests/lib/kv_db/` ztest, run via `west twister -p native_sim -T tests/lib/kv_db`.

Cases:

1. `mount_empty_formats_partition` — fresh erase, mount OK, `count()==0`
2. `set_get_roundtrip` — single key, binary value with NUL bytes inside
3. `get_missing_returns_enoent`
4. `overwrite_returns_latest` — set k=A, set k=B, get → B; `count()` unchanged
5. `delete_then_get_enoent`; `delete_missing_enoent`
6. `persistence_across_remount` — set N keys, unmount, mount, all present
7. `boundary_key_len`, `boundary_val_len` — at MAX, MAX+1 (reject), 1, 0 (reject empty key; allow empty value)
8. `crc_corruption_detected` — manually corrupt a byte via `flash_area_write`, re-mount, log truncated cleanly
9. `log_full_triggers_compaction` — fill log; next set causes compaction; data preserved
10. `compaction_drops_tombstones` — set k1, set k2, delete k1, compact, scan flash → only k2 present
11. `mount_picks_higher_generation` — manually seed both halves with different gens, verify selection
12. `clear_resets_db` — populate, clear, mount → empty
13. `iterate_visits_all_live` — callback hit count matches `count()`

For cross-process persistence tests, `testcase.yaml` declares `extra_args: --flash=/tmp/kv_db_test.bin`. Most cases use in-process unmount/mount to exercise the same persistence paths without needing the flash file across runs.

---

## 12. Verification (end-to-end)

1. `west build -b native_sim app -p` — clean build, no warnings.
2. `./build/zephyr/zephyr.exe --flash=/tmp/kv.bin --stop_at=2` — first run prints `boot_count=1`, sets `hello → "world"`. Second run prints `boot_count=2`, reads `hello` back. Delete `/tmp/kv.bin` → resets.
3. `west twister -p native_sim -T tests/lib/kv_db` — all cases green.
4. `xxd /tmp/kv.bin | grep -c KVDH` — at least 1 sector-header match. `grep -c KV` — many entry-header matches.

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
