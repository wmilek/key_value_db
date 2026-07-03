# L1 — `blob_db`: Stable-ID Blob Store on `flash_area` (i-node allocation)

Status: v2 · Target board for v1: `native_sim`
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`
· Lower boundary: `doc/layers/l0_flash.md` · Consumed by: `doc/layers/l2_containers.md`

---

## 1. Scope

This project delivers a single Zephyr library module: **`lib/blob_db`** — an embedded **blob store with stable identifiers**, built directly on Zephyr's `flash_area` API (`<zephyr/storage/flash_map.h>`).

### What it is

`blob_db` stores opaque, variable-length **payloads** addressed by **u64 ids** that the library assigns on insertion. The library does *not* know about strings, keys, schemas, indexes, or queries. It promises only one thing: **once you have an id, you can fetch the payload that was stored under it**.

### What it isn't

`blob_db` is intentionally **not** a key-value store. Indexing (mapping user-meaningful keys — strings, hashes, queries — to ids) is a separate concern owned by the client. Appendix A sketches some client-side indexing patterns; none are implemented here.

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Application                                                │
└─────────────────────────────────────────────────────────────┘
                            │   name → id, query → ids, ...
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  Client index(es)              (built by the application)   │
│    e.g. list / tree / hash of (name → blob id), each node   │
│    persisted as its own blob.                                │
└─────────────────────────────────────────────────────────────┘
                            │   put / get / update / delete  (by u64 id)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  blob_db                       (lib/blob_db)                │
│    - 8 MB partition, hash-bucketed by id                    │
│    - stable u64 ids (never reused)                          │
│    - O(1) lookup, O(1) steady-state RAM                     │
└─────────────────────────────────────────────────────────────┘
                            │   flash_area_*
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  flash_area                    (Zephyr)                     │
└─────────────────────────────────────────────────────────────┘
```

Only the `flash_area` boundary is touched by `blob_db`. A future UBI-style flash-translation layer that presents itself as a `flash_area` provider can slot in below `blob_db` without changing a line of library code.

---

## 2. Stability contract

This is the load-bearing promise of the library — clients build on top of it.

| Property | Guarantee |
|---|---|
| **Id assignment** | `put` returns a u64 id that is currently unused, never previously assigned in the lifetime of this DB, and ≥ the id returned by any prior `put`. |
| **Id stability** | Once an id is returned, it refers to the same logical blob until that id is `delete`d. `update` keeps the id; only the payload changes. |
| **Compaction transparency** | Compaction may move slots within a bucket and may erase tombstones, but **ids never change**. A blob you can `get` today is reachable by the same id after any number of compactions. |
| **No reuse** | After `delete(id)`, that id is never assigned to another blob. The next assigned id is strictly greater than every id ever seen. |
| **Atomicity of single operations** | Each `put`/`update`/`delete` is atomic with respect to crash: either it takes effect fully or it doesn't (on next mount). Partial writes are detected and discarded. |
| **No partial reads** | `get` either returns the complete payload that was committed at some point, or returns `-ENOENT`/`-EIO`. It never returns partial bytes from an in-flight write. |

These together make ids usable as **persistent references** — foreign keys for client-owned indexes, parent/child pointers in client trees, etc.

What this contract means for a client — the exact call ordering that makes
multi-blob structures crash-safe, and which steps can leave unreferenced
blobs — is demonstrated operation by operation in the companion document
`doc/layers/l1_model_container.md` (the *model container*).

### Root convention

The very first successful `put` after a fresh format returns **id = 1**. Clients use id = 1 as their persistent root pointer:

```
First boot:  blob_db_put(initial_root_blob, ..., &id)   →  id = 1
Later:       blob_db_update(1, new_root_blob, ...)      ← keeps id = 1
On mount:    blob_db_get(1, ...)                        ← always finds the latest root
```

Because `update` is id-stable, id = 1 always names "the latest version of the client's root". No extra library API needed; convention does it.

---

## 3. Design constraints (carried forward)

These constraints from earlier iterations still hold:

- **Minimum steady-state RAM.** Total RAM owned by `blob_db` between calls is ~32 bytes (partition handle, master generation, master state, next-id counter, compaction state). **No per-blob RAM.** No caching.
- **No linear scan of the database on lookup.** `get(id)` reads exactly one flash sector (the id's hash bucket) and scans only that sector in RAM.
- **Flash reads are fast.** Algorithms favor read-heavy work over write-heavy.
- **Scale: 100 000 blobs in 8 MB.** No structural limit; only partition size.
- **Crash-safe.** Every operation either commits or is invisible on next mount.

### Cost summary (per operation, in flash reads / writes)

| Op | Flash reads | Flash writes | RAM (transient) |
|---|---|---|---|
| `mount` | 2 (master A + B) + 2045 (bucket header scan)¹ | 0 (or formatting if first ever) | O(1) |
| `put(payload, len)` | 0 | 1 (entry) + occasional bucket-compact | 4 KB stack only during compact |
| `get(id)` | **1** (one bucket sector) | 0 | 4 KB stack |
| `update(id, payload, len)` | 0 (or 1 to verify existence)² | 1 (new entry; old becomes garbage) + occasional compact | 4 KB stack |
| `delete(id)` | 1 (verify presence) | 1 (tombstone) | 4 KB stack |
| `exists(id)` | 1 | 0 | 4 KB stack |
| `count` | 2045 (full bucket scan) | 0 | O(1) |
| `iterate` | 2045 | 0 (callback may iterate) | O(1) |
| `compact_bucket` | 1 + master writes | 1 scratch + 1 bucket restore + 2 master | O(1) |

¹ Mount scans all buckets to recover per-bucket write cursors and the global max id. ~8 MB of reads; ≈100–200 ms on real flash.
² `update` can skip the existence check at the cost of allowing "update a deleted id to undelete it" semantics; choose strict-by-default.

---

## 4. Storage layout

Partition: 8 MB = 2048 × 4 KB sectors (assuming the `native_sim` overlay or equivalent real-hardware DT). One sector = one PEB (erase block) for native_sim; on different flash, "PEB" maps to the erase block from `flash_area_get_sectors`.

```
sector  0       master A          ← metadata, gen counter, compaction state
sector  1       master B          ← double-buffer of master
sector  2       scratch           ← shared compaction scratch
sectors 3..2047 bucket 0..2044    ← N = 2045 hash buckets, 4 KB each
```

A blob with id `i` lives in bucket `i mod N`. With sequential id assignment (1, 2, 3, …), the round-robin lands each new id in the next bucket — perfectly uniform distribution. Per-bucket mean entries at 100k blobs = 49.

---

## 5. On-flash format

### 5.1 Master sector (24 B header, the rest of the 4 KB sector is unused / reserved)

```
magic[4]        = 'B','D','M','S'           /* blob db master */
generation[4]   = monotonic LE              /* latest-gen master wins */
state[1]        = CLEAN | COMPACTING
compacting_bid[2] = bucket id if state==COMPACTING else 0
reserved[1]
next_id_hint[8] = max id ever assigned, persisted occasionally  /* see §7.1 */
hdr_crc32[4]    = CRC32-IEEE over preceding 20 B
```

Master is **double-buffered**: writes alternate between sector 0 and sector 1, generation increments. On mount, the master with the higher valid generation wins. A torn master write loses the new gen — the previous one still wins.

### 5.2 Bucket layout (one sector, 4 KB)

```
offset 0x000   Bucket header (16 B)
offset 0x010   Entry slot stream  (append-only)
offset ...     (rest erased = 0xff)
```

### 5.3 Bucket header (16 B)

```
magic[4]        = 'B','D','B','H'           /* blob db bucket header */
bucket_id[2]    = sanity check
reserved[2]
gen[4]          = bucket's own generation, ++ on each in-place rewrite via compact
hdr_crc32[4]
```

The bucket header is written once after the sector is erased (either on first format or on each compact). Subsequent inserts just append entry slots after the header.

### 5.4 Entry slot

Variable length, write-block-aligned:

```
struct blob_slot_hdr {           /* 4 B, __packed */
    uint8_t  flags;              /* bit0 SEALED | bit1 TOMBSTONE */
    uint8_t  reserved;
    uint16_t val_len;            /* 0..MAX_PAYLOAD_LEN, LE */
};
/* followed by: */
/*   uint64_t id           (LE)                                          */
/*   uint8_t  payload[val_len]   (omitted entirely if TOMBSTONE)         */
/*   uint16_t crc16_ccitt  (LE)  CRC over hdr + id + payload             */

slot_size(val_len) = round_up(4 + 8 + val_len + 2, W)
                   = round_up(14 + val_len, W)              W = flash_area_align()
```

Header overhead is 14 B per slot. A typical tombstone is `round_up(14 + 0, W)` = 14 B (or 16 B at W=8).

### 5.5 End-of-bucket detection

The end of the bucket's slot stream is the first offset where any of:

- slot would extend past the sector end, **or**
- `val_len > MAX_PAYLOAD_LEN`, **or**
- `flags == 0xff` (erased — slot was never written), **or**
- CRC fails when read.

The write cursor for that bucket is set to that offset. Erased flash naturally trips the `flags == 0xff` test.

---

## 6. Slot semantics (the latest-wins rule)

A bucket's slot stream is an append-only log of operations on the ids that hash to that bucket. Within a bucket:

- **Multiple slots may exist for the same id** (after updates and tombstones).
- A `get(id)` finds the **last** slot in append order matching that id.
- If the last matching slot is a TOMBSTONE, the id is deleted: return `-ENOENT`.
- Otherwise it's the current value: return the payload.

Compaction (§7.6) removes overridden and tombstoned slots so that each id has at most one slot remaining.

---

## 7. Algorithms

### 7.1 Mount

```
fa = flash_area_open(partition)

# Pick the authoritative master
read master A, master B
  - validate magic + CRC of each
pick the one with higher generation (or the only valid one)
master_gen = chosen.generation
master_state = chosen.state
state_bucket = chosen.compacting_bid
next_id_hint = chosen.next_id_hint

# Crash-recover any mid-compaction (§8)
if master_state == COMPACTING:
    rebuild bucket `state_bucket` from scratch sector if scratch is sealed,
    else discard scratch.
    write a new CLEAN master to the other slot, gen+1.

# Recover per-bucket state.
# Scan every bucket sector to recover its write cursor and observe the max id.
max_id_seen = next_id_hint
for bid in [0 .. N):
    open bucket bid
    if bucket header invalid: bucket is "unformatted"; will format on first put
    else:
        cursor = 0x10
        while cursor + sizeof(slot_hdr) <= sector_end:
            read slot_hdr at cursor
            if invalid: break               # end of log
            id = read 8 bytes at cursor+4
            if not (flags & TOMBSTONE):
                max_id_seen = max(max_id_seen, id)
            cursor += slot_size(val_len)
        write_cursor[bid] = cursor

next_id = max_id_seen + 1
```

Total mount cost: ~2 master reads + 2045 sector reads ≈ 8 MB of reads. The bucket scans read full 4 KB sectors (not just headers) because each slot's `val_len` is needed to skip to the next slot. ≈100–200 ms on typical NOR.

### 7.2 `put(payload, len) → id`

```
id = next_id++
bid = id % N

ensure bucket bid is formatted (write bucket header if first use)
slot_sz = round_up(14 + len, W)
if write_cursor[bid] + slot_sz > sector_end:
    compact_bucket(bid)
    if still no room: return -ENOSPC

build slot in stack buffer (slot_sz ≤ 14 + MAX_PAYLOAD_LEN + W bytes)
flash_area_write(fa, bucket_offset(bid) + write_cursor[bid], buf, slot_sz)
write_cursor[bid] += slot_sz

# occasionally persist next_id (e.g. every 256 inserts) to bound mount scan:
if (next_id & 0xff) == 0:
    persist next_id_hint = next_id via master write

return id
```

No duplicate-id check is needed — `next_id` is monotonic.

### 7.3 `get(id, out, out_sz, *out_len)`

```
bid = id % N
read bucket bid's sector entirely into a 4 KB stack buffer
                                  ── one flash_area_read, the only flash I/O ──

walk slots from offset 0x10:
    track latest_slot for this id (latest in append order)
if latest_slot is None        → return -ENOENT
if latest_slot is TOMBSTONE   → return -ENOENT
if out_sz < latest_slot.val_len → return -ENOMEM
copy latest_slot.payload to out
*out_len = latest_slot.val_len
return 0
```

Single flash read; in-RAM scan of one sector. Independent of total entry count.

### 7.4 `update(id, payload, len)`

```
verify id exists (use get-shaped scan; can be skipped if caller asserts)
slot_sz = round_up(14 + len, W)
bid = id % N

if write_cursor[bid] + slot_sz > sector_end: compact_bucket(bid)
if still no room: return -ENOSPC

build slot with the SAME id, SEALED flag
flash_area_write(fa, bucket_offset(bid) + write_cursor[bid], buf, slot_sz)
write_cursor[bid] += slot_sz
return 0
```

The previous slot for this id becomes garbage (an "overridden" slot, identical mechanism to a `put` followed by another `put` of the same id). Compaction reclaims it later.

### 7.5 `delete(id)`

```
verify id exists (latest non-tombstone slot in bucket)
if not exists: return -ENOENT

slot_sz = round_up(14 + 0, W)
if write_cursor[bid] + slot_sz > sector_end: compact_bucket(bid)
if still no room: return -ENOSPC

append a TOMBSTONE slot with this id, val_len=0
write_cursor[bid] += slot_sz
return 0
```

A tombstone is a normal slot with `flags & TOMBSTONE` set and `val_len = 0`. It marks the id deleted on the next `get`/`update`. Compaction eventually erases both the tombstone *and* every earlier slot for that id.

### 7.6 `compact_bucket(bid)` — per-bucket compaction

Invariant: after compaction, the bucket contains exactly one slot per **live** id (no tombstones, no overrides), in some order, with sealed slots only.

```
# Phase 1: read the bucket; compute live set in RAM, write to scratch
read bucket bid's sector  →  4 KB stack buffer

# Live = latest-non-tombstone slot for each id present
walk slots, build a small list of (id, src_offset, val_len) entries to keep

# Build the new bucket image in a second 4 KB stack buffer:
new_buf[0x10] = bucket header (gen = old_gen + 1)
new_cursor = 0x10
for each kept (id, off, val_len) in oldest-first order:
    copy slot from old buffer to new_buf at new_cursor
    new_cursor += slot_size(val_len)

# Phase 2: persist via master + scratch + erase + restore + master
write master B  (gen+1, state=COMPACTING, compacting_bid=bid)        ─┐
flash_area_erase(scratch sector)                                      │
flash_area_write(scratch sector, new_buf, new_cursor)                 │  ATOMIC
flash_area_erase(bucket bid's sector)                                 │  WINDOW
flash_area_write(bucket bid's sector, new_buf, new_cursor)            │
flash_area_erase(scratch sector)                                      │
write master A  (gen+2, state=CLEAN, compacting_bid=0)               ─┘

# Update in-RAM state
write_cursor[bid] = new_cursor
```

The atomic window is bracketed by master writes. Any crash inside it is recovered on next mount (§8).

### 7.7 `iterate(cb, user)`

```
for bid in [0..N):
    read bucket bid's sector
    walk slots; for each id with latest non-tombstone slot:
        cb(id, payload_ptr, val_len, user)
```

Order is bucket-by-bucket (not id-sorted). 2045 flash reads total; callback runs in-line with each sector buffer still resident.

---

## 8. Atomicity & crash recovery

The library has exactly **two** atomic-commit primitives:

1. **Single slot append** — `flash_area_write` of a complete slot is the commit. Torn writes are detected by CRC and the slot is treated as not-present on the next scan.
2. **Master sector update** — a single sector write to the inactive master slot. Torn → CRC fails → the *other* master sector with the lower gen still wins.

A `compact_bucket` is multi-step but bracketed by master writes that encode the recovery plan.

### 8.1 Crash table

| Crash point in `compact_bucket(bid)` | Mount sees | Recovery |
|---|---|---|
| Before any write | Both master sectors say CLEAN. | Nothing to do. |
| During master-B write (begin compact) | Master B CRC fails. Master A still says CLEAN. | Treat as not-yet-compacting. |
| After master B sealed; during scratch erase/write | Master B says COMPACTING(bid). Scratch sector has bad CRC or wrong magic. | Discard scratch. Bucket `bid` is original. Write master A CLEAN. |
| After scratch sealed; during bucket erase | Master B = COMPACTING(bid). Scratch is sealed & valid. Bucket `bid` may be partially erased or pre-erase. | Copy scratch → bucket bid (full sector). Erase scratch. Write master A CLEAN. |
| After bucket restore; during scratch erase | Same as previous; bucket is already restored. | Erase scratch. Write master A CLEAN. |
| During final master-A write | Master B still says COMPACTING(bid), but bucket+scratch state may be either. | Re-run the recovery: if scratch valid, copy → bucket; otherwise bucket already done. Then write master A CLEAN. (Idempotent.) |

### 8.2 Single-slot crash

A torn slot write leaves either:

- Some bytes of the slot, with `flags`-byte still 0xff → mount scan detects "erased" → treats as end-of-log, sets write_cursor here. **Tail garbage occupies space until next compaction**, which reclaims it.
- Slot looks structurally valid but CRC fails → same: end-of-log here.

Either way, no committed slot is corrupted, and the bucket falls back to a self-consistent state.

---

## 9. Public API (`include/app/lib/blob_db.h`)

```c
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

int      blob_db_mount(void);
int      blob_db_unmount(void);

/* Returns -ENOSPC if no bucket has room even after compaction. */
int      blob_db_put   (const void *payload, size_t len, uint64_t *out_id);

/* Strict: returns -ENOENT if id was never assigned or was deleted. */
int      blob_db_get   (uint64_t id, void *out, size_t out_sz, size_t *out_len);
int      blob_db_update(uint64_t id, const void *payload, size_t len);
int      blob_db_delete(uint64_t id);
bool     blob_db_exists(uint64_t id);

size_t   blob_db_count(void);

typedef int (*blob_db_iter_cb_t)(uint64_t id,
                                  const void *payload, size_t len,
                                  void *user);
int      blob_db_iterate(blob_db_iter_cb_t cb, void *user);

/* Format the partition (erase all blobs, reset next_id to 1). Intended for
 * factory tests / explicit reset. */
int      blob_db_format(void);
```

Errors: `-ENOENT`, `-ENOSPC`, `-ENOMEM`, `-EINVAL`, `-EIO`, `-ENODEV` (not mounted), `-EALREADY` (double mount).

**Concurrency contract (v1):** single-threaded — caller serializes. Documented in the header. v2 may add a `k_mutex`.

---

## 10. Kconfig

`lib/blob_db/Kconfig` (sourced from the existing `lib/Kconfig` menu):

```
BLOB_DB                     bool   "Stable-id blob storage library"
                            select FLASH
                            select FLASH_MAP
                            select CRC
BLOB_DB_PARTITION_LABEL     string default "storage"
BLOB_DB_MAX_PAYLOAD_LEN     int    default 256   range 1 4096
module = BLOB_DB / module-str = BLOB_DB    /* standard LOG module pattern */
```

No max-key-len (no keys). No index capacity (no in-RAM index). Partition geometry is discovered at runtime from `flash_area_get_size` and `flash_area_get_sectors`.

---

## 11. Repo integration

Mirrors the existing `lib/custom/` pattern.

```
lib/
  CMakeLists.txt              add_subdirectory_ifdef(CONFIG_BLOB_DB blob_db)
  Kconfig                     rsource "blob_db/Kconfig"   (inside existing menu)

  blob_db/
    CMakeLists.txt
    Kconfig
    blob_db.c                 public API + bucket / compaction logic
    blob_db_internal.h        on-flash structs, BUILD_ASSERTs

include/app/lib/
  blob_db.h                   public API

tests/lib/blob_db/
  CMakeLists.txt
  prj.conf
  testcase.yaml
  src/main.c                  ztest

app/
  boards/native_sim.overlay   16 MB sim-flash; 8 MB storage_partition @ 0x800000
  prj.conf                    CONFIG_BLOB_DB=y, CONFIG_FLASH=y, CONFIG_FLASH_MAP=y,
                              CONFIG_CRC=y, CONFIG_LOG=y
  src/main.c                  demo: increment a "boot count" via the id=1 root
```

---

## 12. Zephyr APIs used

- `<zephyr/storage/flash_map.h>` — `FIXED_PARTITION_ID`, `flash_area_open`, `flash_area_close`, `flash_area_read`, `flash_area_write`, `flash_area_erase`, `flash_area_get_size`, `flash_area_get_sectors`, `flash_area_align`
- `<zephyr/sys/crc.h>` — `crc16_ccitt(0xffff, …)` for slots; `crc32_ieee(…)` for master & bucket headers
- `<zephyr/logging/log.h>` — module logger

---

## 13. Failure-mode summary

| Scenario | Outcome |
|---|---|
| Crash mid-slot write | Slot CRC fails or `flags`-byte still 0xff → next scan treats as end-of-log; previous committed slots intact. |
| Crash mid-compaction (any phase) | Recoverable via master state machine + scratch sector (§8.1). Bucket reverts to pre-compaction state or completes; ids unaffected. |
| Crash mid-master-update | Older master with valid CRC still wins; the new attempt is invisible. |
| Bit corruption in a slot | CRC catches it on read; slot treated as end-of-log. Everything *after* that bad slot in the same bucket is lost until next compaction. |
| Partition full | `put` returns `-ENOSPC` after attempting compaction on the destination bucket. Caller deletes or accepts. |
| Bucket overflow at high load factor | Identical to "partition full" for that specific bucket. Round-robin id assignment keeps fill uniform, so this happens to all buckets at ~the same time. |
| Id space exhaustion | After 2⁶⁴ puts. Not reachable in practice (would take 5800 years at 100M puts/sec). |

---

## 14. Testing strategy

`tests/lib/blob_db/` ztest, run via `west twister -p native_sim -T tests/lib/blob_db`.

Cases:

1. `mount_empty_formats_partition` — fresh erase, mount OK, `count()==0`, next id will be 1.
2. `put_get_roundtrip` — single blob, payload with NUL bytes inside.
3. `put_returns_id_1_first_then_monotonic` — verifies the root-convention.
4. `get_missing_returns_enoent`
5. `update_keeps_id` — put → get(id) old, update(id) → get(id) new; id unchanged.
6. `delete_then_get_enoent`; `delete_missing_returns_enoent`; `delete_twice_second_enoent`.
7. `update_after_delete_returns_enoent` — strict semantics.
8. `persistence_across_remount` — put N blobs, unmount, mount, all gettable.
9. `boundary_payload_len` — at MAX, MAX+1 (reject), 1, 0 (allow).
10. `corrupted_slot_truncates_bucket` — corrupt a byte mid-bucket, re-mount, that bucket truncates cleanly.
11. `bucket_full_triggers_compaction` — fill one bucket; next put compacts and succeeds.
12. `compaction_drops_tombstones_and_overrides` — put k1 → update k1 → delete k1; compact; bucket has 0 slots for k1.
13. `compaction_preserves_ids` — put 50 blobs, force compaction, get all back by id.
14. `mid_compaction_crash_recovery` — manually corrupt master to simulate COMPACTING state; mount recovers (either to pre- or post-compact state, consistently).
15. `scale_100k` — gated by a Kconfig flag; insert 100 000 small blobs, verify a random subset round-trips. With the 8 MB overlay this completes in a few seconds.

For cross-process persistence tests, `testcase.yaml` declares `extra_args: --flash=/tmp/blob_db_test.bin`. Most cases use in-process unmount/mount to exercise persistence without needing the flash file across runs.

---

## 15. Verification (end-to-end)

1. `west build -b native_sim app -p` — clean build, no warnings. The overlay at `app/boards/native_sim.overlay` is auto-applied.
2. `./build/zephyr/zephyr.exe --flash=/tmp/blob.bin --stop_at=2` — first run formats, puts a "boot count = 1" blob at id=1; second run reads id=1, prints `boot_count=2`, updates id=1. Delete `/tmp/blob.bin` → resets to 1. Flash file size on disk ≈ 16 MB.
3. `west twister -p native_sim -T tests/lib/blob_db` — all cases green.
4. `xxd /tmp/blob.bin | grep -c BDMS` ≥ 1 (master magic); `grep -c BDBH` ≥ 1 (bucket header magic).

---

## 16. Appendix A — Client-side indexing patterns

Indexing is **out of scope** for the library, but here is what callers typically build with the primitives above. None of this code lives in `blob_db`.

### A.1 Linked list of (name → blob id)

Each list node is its own blob. The root (id=1) holds the head id.

```c
struct list_node {
    uint64_t next_id;        /* 0 = end of list                          */
    uint64_t target_id;      /* the data blob this name maps to          */
    uint8_t  name_len;
    char     name[];         /* not NUL-terminated, packed               */
};

struct root_blob {
    uint64_t head_id;        /* first list_node, or 0 if empty           */
};

int my_set(const char *name, const void *val, size_t len) {
    uint64_t data_id, node_id;
    blob_db_put(val, len, &data_id);

    struct root_blob r;
    size_t rl;
    blob_db_get(1, &r, sizeof(r), &rl);

    struct list_node *n = stack_build(r.head_id, data_id, name);
    blob_db_put(n, n_total_size(n), &node_id);

    r.head_id = node_id;
    return blob_db_update(1, &r, sizeof(r));
}
```

O(n) lookup. Fine for small (~tens) name spaces.

### A.2 Balanced tree (e.g. AVL or radix)

Each tree node is a blob holding `(left_child_id, right_child_id, key, target_id)`. Root id stored in the same id=1 root blob. Lookups are O(log n) **flash reads** — one per tree level. Updates touch the path from root to leaf and require the parent chain to be rewritten (copy-on-write).

For 100k entries with branching factor 8, depth ≈ 6 → 6 flash reads per lookup. Still very fast.

### A.3 Hash table

Each bucket of the client's hash table is a blob. Lookup: hash → bucket id → fetch bucket blob → scan in-RAM. The client decides the resolution and resize policy.

This duplicates `blob_db`'s own bucket structure at a higher level — usually overkill, but useful if the client wants string keys with O(1) lookup.

### A.4 No index at all

For small databases (<100 entries), the simplest pattern is `iterate(cb)` plus an in-callback scan. The library's `iterate` is O(N) but the constant factor is low (one sector read per ~50 entries), so for small N it's effectively free.

---

## 17. Forward-compat (UBI)

A future UBI-style flash-translation layer will present a `flash_area`-shaped interface upstream. When that ships, `blob_db` gets pointed at UBI's virtual partition instead of the platform's `storage_partition` (via `BLOB_DB_PARTITION_LABEL`). No source change in `blob_db`.

---

## 18. Critical files (relative to repo root)

- `lib/blob_db/blob_db.c` — implementation
- `lib/blob_db/blob_db_internal.h` — on-flash structs, magic constants, asserts
- `lib/blob_db/Kconfig` — options
- `lib/blob_db/CMakeLists.txt`
- `include/app/lib/blob_db.h` — public API
- `lib/CMakeLists.txt`, `lib/Kconfig` — registration
- `app/prj.conf`, `app/src/main.c` — enable + demo
- `app/boards/native_sim.overlay` — 16 MB sim-flash, 8 MB storage_partition
- `tests/lib/blob_db/` — ztest suite

---

## 19. Appendix B — Allocation strategy: one interface, exchangeable allocators

### B.1 The interface is the contract, the allocator is an implementation

Everything in §4–§8 (bucket layout, slot format, compaction) describes **one**
allocator — the **bucket-log** allocator, v1's choice. The layer boundary is
only the public API (§9) plus the stability contract (§2). Alternative
allocators may replace bucket-log behind the same API, selected at build time:

```
choice BLOB_DB_ALLOCATOR
  config BLOB_DB_ALLOC_BUCKETLOG      # v1, this document
  config BLOB_DB_ALLOC_FAT            # fixed-size chunks + bitmap/chain (future)
  config BLOB_DB_ALLOC_EXTENT         # extent lists (future)
endchoice
```

**Swap policy:** on-flash formats are mutually incompatible. Changing the
allocator means a reformat — all stored data (and therefore every L2/L3
structure) is lost. This is *accepted*: code above L1 needs no change, data is
not migrated. Each allocator uses its own master magic so a mismatched mount
fails cleanly with `-ENOTSUP` instead of misreading another format.

### B.2 Candidate allocators compared

| | **bucket-log (v1)** | **FAT-like** | **extent-based** |
|---|---|---|---|
| Allocation unit | variable-length slot | fixed-size chunk | variable extent (start, len) |
| id → data | computed: `id mod N` → bucket, scan | i-node table → chunk chain | i-node table → extent list |
| Free-space accounting | per-bucket append cursor | chunk bitmap | free-extent search |
| Max payload | one sector (§5.4) | unbounded (chain) | unbounded (multi-extent) |
| Fragmentation | none internal; garbage slots until compact | internal: ~½ chunk per blob | external: free-space splinters |
| GC | per-bucket compaction | still needed (B.3) | compaction w/ extent moves |
| Suits | many small blobs (L2 nodes) | mixed sizes, simple bookkeeping | few large blobs, streaming |

### B.3 Are fixed-size slots worth it?

What fixed chunks **buy**: O(1) bitmap accounting, uniform reads, no
variable-slot scan. What they **don't** buy: in-place update. Flash erases at
erase-block granularity, so a "freed" bitmap bit cannot be set back to free
without erasing its whole block — updates stay out-of-place and erase-block GC
remains necessary. A FAT-like allocator on flash ends up log-structured at
block level anyway.

What they **cost**: internal fragmentation. The dominant workload is small L2
container nodes (tens–hundreds of bytes); with 64 B chunks, ~32 B average tail
waste × 100 k blobs ≈ 3 MB of an 8 MB partition. Verdict: for v1's workload,
variable-length slots waste strictly less; fixed chunks only pay off once
blobs regularly exceed one sector — which v1 forbids anyway (§5.4).

### B.4 Multi-chunk payloads and the read interface

Chunking can live at two levels, and v1 deliberately puts it at L2: large data
is chained from `seq` container chunk i-nodes (`l2_containers.md` §4.1, used by
blobfs file bodies). L1 stays single-chunk.

If a future allocator (FAT/extent) spreads payloads transparently, the
whole-blob `get()` becomes inadequate — it forces the caller to buffer the
entire blob. The agreed extension is **pread-style partial access**,
implementable by every allocator (single-chunk included), so it can be added
without ever changing again on an allocator swap:

```c
/* Total committed payload size, or -ENOENT. */
int blob_db_size(uint64_t id, size_t *out_size);

/* Read len bytes starting at offset; short read at end of blob.
 * Reads only the chunks covering [offset, offset+len). */
int blob_db_read(uint64_t id, size_t offset, void *out, size_t len,
                 size_t *out_read);
```

`get()` remains as the `size` + `read(0, size)` convenience. The §2 contract is
unchanged: a multi-chunk write commits by writing the id's index record
**last** (single commit point), so `read` never observes a partially committed
chain — "no partial reads" still holds. Writes stay whole-blob
(`put`/`update` take the complete payload); a streaming write API would need
handles/transactions and is out of scope until a concrete consumer appears.
