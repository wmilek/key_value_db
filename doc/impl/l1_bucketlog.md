# Implementation design — bucket-log allocator for `blob_db`

Status: v2 · **Non-normative feasibility design.** This document proves the
L1 contract (`doc/layers/l1_blob_db.md`) is implementable and guides the
first implementation. Everything here — formats, algorithms, costs — may be
fine-tuned during implementation as long as the contract holds. Upper layers
must not depend on anything in this document (P6). Known deltas against the
contract are tracked in §13 *Open implementation items*.

Target board for bring-up: `native_sim`.

---

## 1. Approach & cost summary

One erase sector = one **bucket**; a blob with id `i` lives in bucket
`i mod N`. Buckets are append-only slot logs; per-bucket compaction reclaims
garbage; a double-buffered master sector carries generation, compaction
state, and the id hint.

Costs achieved by this design (satisfying contract §3):

| Op | Flash reads | Flash writes | RAM (transient) |
|---|---|---|---|
| `mount` | 2 (master A + B) + 2045 (bucket scan)¹ | 0 (or format if first ever) | O(1) |
| `alloc_id()` | 0 | 0 (occasional id-hint persist) | O(1) |
| `get(id)` | 1 bucket header + 12 B per slot + 1 payload | 0 | O(1) |
| `update(id, …)` | 1 bucket header + 12 B per slot² | 1 (slot; on rebind the old slot becomes garbage) + occasional compact | O(1) |
| `delete(id)` | as `get` (verify presence) | 1 (tombstone) | O(1) |
| `exists(id)` | as `get` | 0 | O(1) |
| `count` / `iterate` | 2045 (full scan) | 0 | O(1) |
| `compact_bucket` | 1 + master writes | 1 scratch + 1 bucket restore + 2 master | O(1) |

¹ Mount scans all buckets to recover write cursors and the max id — ~8 MB of
reads, ≈100–200 ms on typical NOR (full sectors are read because each slot's
`val_len` is needed to skip to the next slot).
² Release builds perform no existence check — `update` on a dead id is UB
(contract §2). A debug build may spend one bucket read to verify and assert
(§13.5).

## 2. Storage layout

Partition: 8 MB = 2048 × 4 KB sectors (`native_sim` overlay). One sector =
one erase block ("PEB"); on other flash, geometry comes from
`flash_area_get_sectors`.

```
sector  0       master A          ← metadata, gen counter, compaction state
sector  1       master B          ← double-buffer of master
sector  2       scratch           ← shared compaction scratch
sectors 3..2047 bucket 0..2044    ← N = 2045 hash buckets, 4 KB each
```

Sequential id allocation round-robins ids across buckets — uniform fill.
Per-bucket mean at 100 k blobs: 49 entries.

## 3. On-flash format

### 3.1 Master sector (64 B header; rest of the sector reserved)

Two parts: a frozen compatibility prefix that any version can parse, and a
body that belongs to the current format major.

```
/* --- frozen prefix (12 B) — this layout never changes ------------------- */
magic[4]         = 'B','D','M','S'          /* allocator identity (D1)      */
format_major[1]  = 1                        /* incompatible; refuse unknown */
format_minor[1]  = 0                        /* additive; safe to ignore     */
hdr_len[2]       = 64                       /* total header length          */
reserved[2]
prefix_crc16[2]  = CRC16-CCITT(0xffff) over the preceding 10 B

/* --- body (format major 1) ---------------------------------------------- */
generation[4]    = monotonic LE             /* latest valid gen wins        */
state[1]         = CLEAN | COMPACTING
compacting_bid[2]
reserved0[1]
next_id_hint[8]  = id-counter persistence   /* see §5.1 and §13.1           */
reserved1[32]                               /* future MINOR revisions       */
hdr_crc32[4]     = CRC32-IEEE over bytes [0, 60)
```

Double-buffered: writes alternate between sectors 0/1 with incrementing
generation; a torn master write loses the new generation and the previous
master still wins.

**Two CRCs, and the split is load-bearing.** A *newer* writer still produces a
prefix whose CRC verifies, so an older reader can trust `format_major` and
refuse the store deliberately. Bit rot produces a prefix whose CRC fails —
a different situation, in which the other master may still be good. One
combined CRC would conflate them, and worse, would move as the header grows,
so an older reader could not even locate it.

The body is a fixed 64 B with reserved padding so `hdr_crc32` stays at a
constant offset over a constant span. A MINOR revision consumes reserved space
and moves nothing; older software validates the same span and reads the fields
it knows. Anything that does not fit — or that older software would *misread*
rather than merely miss, such as a new slot flag — is a MAJOR change instead.

**Mount classifies each master sector**, and only one of the four outcomes may
write to flash:

| Sector reads as | Class | Mount |
|---|---|---|
| all `0xff` across the header window | erased | virgin — format |
| prefix CRC fails | corrupt | ignore it; the other master may be good |
| prefix verifies, foreign magic or unknown major | **foreign** | `-ENOTSUP`, touch nothing |
| prefix and body verify | ok | candidate; highest generation wins |

A foreign sector on *either* slot refuses the whole store, before generation
is considered: an interrupted upgrade can leave the old format on one slot and
a newer one on the other, and falling back to the older would silently mount a
stale view. When neither master is usable and neither is erased,
`BLOB_DB_AUTOFORMAT_ON_CORRUPT` decides between reformatting (development) and
`-EIO` (production). Recovering that way rewrites the masters but does not
erase buckets, so surviving blobs stay readable and mount's defensive scan
(§5.1) re-raises the id ceiling past them.

### 3.2 Bucket layout (one sector)

```
offset 0x000   bucket header (16 B):  magic 'BDBH', bucket_id[2], reserved[2],
                                      gen[4] (++ per compact), hdr_crc32[4]
offset 0x010   entry slot stream (append-only)
...            rest erased = 0xff
```

The header is written once per erase (first format or each compact); inserts
append slots after it.

### 3.3 Entry slot

Variable length, write-block aligned (`W = flash_area_align()`):

```
struct blob_slot_hdr {           /* 4 B, __packed */
    uint8_t  flags;              /* bit0 SEALED | bit1 TOMBSTONE */
    uint8_t  reserved;
    uint16_t val_len;            /* 0..MAX_PAYLOAD_LEN, LE */
};
/* uint64_t id (LE) · payload[val_len] (absent if TOMBSTONE) · crc16_ccitt */

slot_size(val_len) = round_up(14 + val_len, W)
```

### 3.4 End-of-log detection

The slot stream ends at the first offset whose **header** cannot be trusted to
size its own slot: `flags == 0xff` (erased), SEALED clear, a flag bit this
version does not know, `val_len > MAX_PAYLOAD_LEN`, or a slot that would cross
the sector end. The bucket's write cursor is that offset.

A failed **CRC** deliberately does *not* end the log. The header alone gives the
stride to the next slot, so a rotten slot is stepped over and the slots after it
stay reachable; only the rotten slot itself is invisible, and the next
compaction drops it. (The earlier resident-buffer walk verified every CRC as it
went and treated a failure as end-of-log, which hid every later slot in that
bucket until compaction.) Readers verify the CRC of the one slot they actually
want, and fall back to the newest intact slot below it — so the visible value is
always a committed one.

### 3.5 Large objects — segments and an index record

With `CONFIG_BLOB_DB_LARGE_PAYLOADS`, a payload too big for one slot is stored
as K **segment** slots plus one **index** slot at the object's own id. Both are
ordinary slots, so latest-wins, tombstones, `id mod N` addressing and per-bucket
compaction apply to them unchanged; only four call sites know the difference.

```
index slot   flags = SEALED|INDEXED, at the object's id
  payload:   magic 'BX', version, flags, total_len[4], payload_crc32[4],
             seg_count[2], seg_len[2], then seg_id[seg_count] (u64 each)

segment slot flags = SEALED|SEGMENT, at an internal id
  payload:   owner_id[8], seq[2], reserved[2], then the chunk bytes
```

`owner_id` is what makes reclaim possible with no RAM index, and it sits in the
payload rather than the slot header so the header stays 4 bytes for the millions
of small slots that will never be segments.

Introducing these flags is a **MAJOR** format change (§3.1): older software
would read an index record as though it were the object's payload. A build
without the feature keeps writing major 1; a build with it writes major 2 and
reads either.

**Capacity is bounded by the index record**, which is itself a single-slot
payload: `seg_count <= (MAX_PAYLOAD_LEN - 16) / 8`, and the reachable object
size is that times `seg_len`. This is why enabling the feature raises the
default `MAX_PAYLOAD_LEN`, and why mount validates the chain and refuses a
configuration that cannot reach `MAX_OBJECT_LEN` — naming the shortfall rather
than silently capping. `seg_len` defaults to `sector/4` clamped to what a slot
can hold, which leaves several segments plus rebind headroom per bucket.

**Segment placement is a probe sequence.** A segment's id is internal and
arbitrary, and `bucket = id mod N`, so when the bucket a fresh id lands in has
no room the writer simply allocates another id and lands elsewhere. A
user-visible id cannot do this — its bucket is fixed — but segments are never
user-visible.

## 4. Slot semantics — latest wins

A bucket is an append-only log of operations on the ids hashing to it.
Multiple slots may exist for one id (rebinds, tombstone); `get` takes the
**last** matching slot in append order; a trailing TOMBSTONE means dead
(`-ENOENT`). Compaction (§5.6) leaves at most one live slot per id.

## 5. Algorithms

### 5.1 Mount

```
read master A, B → pick higher valid generation
if state == COMPACTING: finish/abort per crash table (§6.1), write CLEAN master

max_id_seen = next_id_hint
for bid in [0..N):
    walk slots: max_id_seen = max(max_id_seen, slot.id)   # tombstones count too
    write_cursor[bid] = end of log
next_id = max_id_seen + 1                                  # but see §13.1
```

### 5.2 `alloc_id`

```
id = next_id++
if (next_id & 0xff) == 0: persist next_id_hint via master write   # see §13.1
return id
```

### 5.3 `update` — first bind and rebind (same append path)

```
bid = id % N
ensure bucket formatted (header write on first use)
if write_cursor[bid] + slot_size(len) > sector_end:
    compact_bucket(bid); if still no room: return -ENOSPC
build slot in stack buffer; flash_area_write at write cursor
```

On rebind the previous slot for the id becomes garbage until compaction.
Debug builds may precede this with a get-shaped scan asserting the id is
bound or freshly allocated (§13.5).

### 5.4 `get`

Walk the bucket by slot header (12 B per slot: header + id), tracking the
latest match, apply latest-wins (§4); then read and CRC-verify just that slot's
payload. No whole-sector read — the point of the header-driven walk is that
answering one lookup costs O(slots) small reads instead of one erase-block read,
which on 64 KB sectors is the difference between ~12 B per slot and 64 KB.

`count` / `iterate` and compaction keep working on a resident sector image:
their liveness test is "does a later slot share this id?", which is O(n²) in
slots, and turning each comparison into a flash read would cost far more than
the sector read it replaced. They are diagnostics and maintenance, not the hot
path (contract D5).

### 5.5 `delete`

Verify presence (1 read); append a TOMBSTONE slot (`val_len = 0`).

### 5.6 `compact_bucket(bid)`

Build the compacted image (live slots only) in RAM, then:

```
write master B  (gen+1, COMPACTING, bid, hint↑)  ─┐   hint↑: cover the highest
erase scratch; write new image to scratch          │   id being dropped, so an
seal scratch (trailer, written last — §6.1)        │   erased tombstone cannot
erase bucket;  write new image to bucket           │   unprotect its id
erase scratch                                      │   (subsumed by §13.1)
write master A  (gen+2, CLEAN)                    ─┘
```

The window is bracketed by master writes; any crash inside is recovered at
mount (§6.1).

### 5.6a Segmented `update` / `delete`, and the sweep

```
update(id, payload, len) with len > MAX_PAYLOAD_LEN:
  0. capture the outgoing index's seg_id[] (if the id holds one)
  1. master: seg_owner = id                      ── enters the sweep window
  2. for j in 0..K-1: alloc an internal id, append a SEGMENT slot
  3. append the INDEXED slot at id                ── ATOMIC COMMIT
  4. tombstone the outgoing segments
  5. master: seg_owner = 0                        ── leaves the sweep window

delete(id) holding an index: same window, tombstone the index at step 3.
```

Step 3 is one slot append — the primitive that already makes a small write
atomic. Before it the new segments are unreferenced; after it the old ones are.
So contract §2 "atomicity" and "no partial reads" hold with nothing new.

**One rule reclaims every crash point:** *a SEGMENT slot whose owner's live
index does not list it is garbage.* Crash in step 2 → the new segments are not
listed. Crash inside step 3 → the slot fails its CRC, so the old index is still
live and the *new* segments are unlisted. Crash in step 4 → the old segments are
not listed. In each case the losing generation is exactly the unlisted set.

Mount runs the sweep only when the master carries a non-zero `seg_owner`, so the
clean path pays nothing. The sweep walks every bucket by slot header, and for
each live SEGMENT slot owned by that id checks the owner's index — O(N) sector
reads, O(1) RAM beyond the segment tables, idempotent, and restartable after a
crash during the sweep itself.

### 5.7 `iterate` / `count`

Bucket-by-bucket full scan; callback runs against the resident sector buffer.
Order is not id-sorted.

### 5.8 Batch operations (optional, `CONFIG_BLOB_DB_MULTI`)

Contract §4 / decision D6: unordered set, per-element result, no cross-element
atomicity, the caller's array is never reordered, O(1) extra RAM (no sort
buffer). The allocator picks its traversal for locality:

```
if n is small (n < THRESHOLD, ~N/8):
    bucket-major — for each distinct bucket among reqs[].id, read the sector
    once, then service every element hashing to it from the resident buffer
else:                                  # k comparable to N
    single full pass — walk buckets 0..N once (like iterate), matching each
    resident sector against the request set
```

Both are O(1) extra RAM and touch each needed bucket exactly once — reads
coalesce strongly (min(k, N) sector reads instead of k). `multi_get` /
`multi_exists` are the strong wins. `multi_update` / `multi_delete` append or
tombstone per element; the write count is unchanged, and the only saving is a
shared sector read (and at most one compaction) when several ids fall in the
same bucket — rare for a collected set (≈ k/N per bucket), so writes benefit
far less than reads. No element ever crosses into another's atomicity: each
slot append / master write is the same single-op commit as the scalar path,
so a crash mid-batch leaves exactly the completed elements (contract §2).

## 6. Atomicity & crash recovery

Two atomic-commit primitives: a **single slot append** (torn → CRC fails →
treated as end-of-log) and a **master sector write** (torn → older master
wins). Compaction composes them.

### 6.1 Compaction crash table

| Crash point | Mount sees | Recovery |
|---|---|---|
| before any write | both masters CLEAN | nothing |
| during master-B write | B invalid, A CLEAN | not-yet-compacting |
| during scratch erase/write | B = COMPACTING, scratch **unsealed** | discard scratch; bucket untouched; write CLEAN |
| during the scratch seal write | B = COMPACTING, seal partial → unsealed | as above; the bucket was never touched |
| during bucket erase/restore | B = COMPACTING, scratch **sealed** | copy scratch → bucket; erase scratch; write CLEAN |
| during final master-A write | B still COMPACTING | re-run recovery (idempotent); write CLEAN |

**"Scratch valid" must mean "the whole image landed", and the bucket header
cannot say that.** The header sits at offset 0 of the image, so it is written
*first*; a write torn anywhere after it leaves a valid header over a truncated
slot stream. Recovery that keyed off the header therefore restored the
truncation over an intact bucket and lost every blob past the tear — a real
data-loss path, fixed by sealing scratch with a trailer written *after* the
image:

```
seal (16 B, at the tail of the scratch sector, written last)
  magic[4]       = 'B','D','S','L'
  image_len[4]   = bytes of compacted image at offset 0
  image_crc32[4] = CRC32-IEEE over those bytes
  seal_crc32[4]  = CRC32-IEEE over the preceding 12 B
```

The seal is at the tail rather than the header being written last so that every
write stays forward-only, which the UBI backend's `ubi_leb_write_at()` mapping
relies on. `compact_commit` refuses an image with no room to seal (`-ENOSPC`),
which is the answer the caller reaches anyway when compaction reclaims nothing.

### 6.2 Torn slot

Torn slot write → `flags` still `0xff` or CRC fails → end-of-log at that
offset; committed slots before it are intact; tail garbage is reclaimed by
the next compaction.

## 7. Kconfig

```
BLOB_DB                        bool, select FLASH, FLASH_MAP, CRC
BLOB_DB_PARTITION_LABEL        string, default "storage"
BLOB_DB_MAX_PAYLOAD_LEN        int, default 256, range 1 65535  # geometry checked at mount
BLOB_DB_SECTOR_BUF_SIZE        int, default 4096
BLOB_DB_AUTOFORMAT_ON_CORRUPT  bool, default y   # see §3.1
BLOB_DB_LARGE_PAYLOADS         bool, default n   # segmented objects (§3.5)
BLOB_DB_MAX_OBJECT_LEN         int, default 131072   # validated at mount
BLOB_DB_MAX_SEGMENTS           int, default 128      # 16 B of .bss each
BLOB_DB_SEGMENT_LEN            int, default 0        # 0 = sector/4, clamped
BLOB_DB_MULTI                  bool, default n   # batch ops (§5.8, contract D6)
module = BLOB_DB (standard LOG pattern)
```

Partition geometry discovered at runtime (`flash_area_get_size/_get_sectors`).

## 8. Repo integration

```
lib/blob_db/{blob_db.c, alloc_bucketlog.c, blob_db_internal.h, Kconfig, CMakeLists.txt}
include/app/lib/blob_db.h
tests/lib/blob_db/          unit suite (this doc §11)
tests/lib/blob_db_contract/ model-container acceptance suite (crash injection)
app/boards/native_sim.overlay   16 MB sim-flash; 8 MB storage_partition
```

## 9. Zephyr APIs used

`<zephyr/storage/flash_map.h>` (open/close/read/write/erase/get_size/
get_sectors/align), `<zephyr/sys/crc.h>` (crc16_ccitt for slots, crc32_ieee
for headers), `<zephyr/logging/log.h>`.

## 10. Failure-mode summary

| Scenario | Outcome |
|---|---|
| Crash mid-slot write | CRC/erased-flags → end-of-log; committed slots intact |
| Crash mid-compaction | master state machine + scratch (§6.1); ids unaffected |
| Crash mid-master write | older valid master wins |
| Bit corruption in a slot | CRC catches it; **only that slot** is unreachable — the header-driven walk steps over it (§3.4) and the next compaction drops it. Readers fall back to the newest intact slot for the id |
| Partition full | `update` returns `-ENOSPC` after attempting compaction of the target bucket |
| One bucket overflows | as above, per-bucket; round-robin allocation keeps fill uniform, so buckets fill together |
| Id space exhaustion | 2⁶⁴ allocations ≈ 5800 years at 100 M/s — not reachable |
| Crash during a segmented write | one rule — an unlisted segment is garbage — covers all three points; mount sweeps when the master carries `seg_owner` (§5.6a) |
| Segment missing or mislabelled on read | `-EIO`; the segment header's `owner_id`/`seq` are checked against what the index claimed, so a stale or foreign slot cannot be served as content |

## 11. Unit-test plan (draft)

`tests/lib/blob_db/`, `west twister -p native_sim`. This list tracks the
draft implementation and will be reworked against the final API; the
*contract-level* coverage lives in the model-container acceptance suite
(`tests/lib/blob_db_contract/`, see `l1_model_container.md` §1).

1. mount on erased partition formats; root id 1 bound with an empty payload
   (`exists(1)`, `count()==1`); first `alloc_id()` returns an id > 1 (the
   concrete value is not contractual)
2. alloc+bind → get round-trip (payload with NUL bytes)
3. ids strictly monotonic; survive remount
4. get/delete on missing id → `-ENOENT`; exists → false
5. rebind keeps id; old content replaced atomically
6. delete → get `-ENOENT`; second delete `-ENOENT`
7. debug-assert on update-after-delete (UB check, debug config only)
8. persistence across remount (N blobs)
9. payload length boundaries (0, 1, MAX, MAX+1 rejected)
10. corrupted slot truncates its bucket cleanly on remount
11. full bucket triggers compaction; bind succeeds after
12. compaction drops tombstones/overrides; preserves all live ids
13. simulated mid-compaction crash (forged COMPACTING master) recovers
14. scale: 100 k blobs, random subset round-trips (Kconfig-gated)
15. batch ops (`BLOB_DB_MULTI`): `multi_get`/`multi_exists`/`multi_delete`
    over a mixed set (present, missing, dead ids) — per-element results
    match the scalar path, array not reordered, and a large batch reads each
    bucket at most once (assert read count ≤ N)

## 12. End-to-end verification

1. `west build -b native_sim app -p` — clean build.
2. Boot-count demo across two runs of `zephyr.exe --flash=…` (note: demo
   currently binds id = 1 directly; to be migrated to a `ROOTREG_KEY('BOOT',0)`
   root — §13.6).
3. `west twister -p native_sim -T tests/lib/blob_db` green.
4. `xxd` the flash file: master magic `BDMS` and bucket magic `BDBH` present.

## 13. Open implementation items (to fine-tune before/during implementation)

1. **Durable id allocation — adopt a leading ceiling.** *(Implemented.)*
   `next_id_hint` on the master is now a **leading ceiling**: an exclusive
   upper bound on every id ever returned by `alloc_id`. `alloc_id` persists a
   fresh ceiling `BLOB_DB_ID_HINT_STEP` (256) ids ahead whenever the current
   one is reached, and mount takes `next_id = next_id_hint` (the bucket scan
   only ever *raises* it defensively, never lowers it). This protects
   allocated-but-unbound ids across a crash — the property the model
   container's watermark recovery and (future) root-registry `get_or_create`
   depend on. It subsumes both earlier monotonicity patches (tombstones toward
   the scan max, the `hint↑` compaction ride-along). Cost: one master write
   per block of 256 allocations. Note: the ceiling never lowers, so `next_id`
   never recovers to a previously issued value after a crash — which is why
   the root convention is realized as a *reserved* id (contract D7): format
   itself consumes and binds id = 1, so no client ever depends on the counter
   recovering to exactly 1, and virgin re-bootstrap is detected by id 1's
   empty payload (rootreg §6), not by re-allocating it.
2. **Steady-state RAM story.** *(Resolved: re-scan.)* The implementation
   keeps **no** `write_cursor[N]` array; each write re-walks the target
   bucket (`walk_bucket`) to find the append cursor (+1 read on
   `update`/`delete`), honoring contract R1 (O(1) steady-state RAM).
3. **Sector-size portability.** *(Resolved.)* The sector buffers are sized by
   `BLOB_DB_SECTOR_BUF_SIZE` and mount refuses a partition whose sector is
   larger. Slot staging moved off the stack into the compaction scratch, so
   the payload cap no longer drives stack depth (contract R7). The cap itself
   is checked against the geometry at mount: a bucket is an append-only log,
   so a rebind needs two slots to coexist, bounding the payload at
   `(sector − 16) / 2 − 14` — about 2026 B on 4 KB sectors, 32 746 B on 64 KB.
4. **Master format version field.** *(Resolved.)* Superseded by the frozen
   compatibility prefix (§3.1): `format_major` / `format_minor` / `hdr_len`
   under their own CRC, so software of any vintage can classify a store it did
   not write and refuse it rather than reformat it.
5. **Debug bound-check for UB `update`** — optional Kconfig
   (`BLOB_DB_ASSERT_BOUND`): get-shaped scan + `__ASSERT` in debug builds,
   nothing in release.
6. **Direct id = 1 users vs the registry** — the app demo (and the Appendix A
   sketches below) bind id = 1 directly, which is legitimate in registry-less
   builds; once the demo image enables the root registry, they move to
   `ROOTREG_KEY` roots.

---

## Appendix A — Client-side indexing patterns (historical)

Superseded by the model container (`l1_model_container.md`) and the container
layer (`l2_containers.md`); kept as informal illustrations of what clients
build on the primitives. Note these sketches predate the root registry and
bind id = 1 directly (§13.6); they also ignore the mutation discipline.

- **A.1 Linked list of (name → blob id)** — each node its own blob; root
  holds the head id; O(n) lookup, fine for tens of names.
- **A.2 Balanced tree** — nodes hold child ids; O(log n) flash reads per
  lookup; copy-on-write path updates.
- **A.3 Hash table** — client bucket blobs; O(1) lookup; client-owned resize
  policy.
- **A.4 No index** — `iterate` + callback scan; effectively free below ~100
  entries.

## Appendix B — Alternative allocators (exploration)

The contract fixes the API; these are candidate implementations behind it
(contract §5.1). Comparison:

| | **bucket-log (this doc)** | **FAT-like** | **extent-based** |
|---|---|---|---|
| Allocation unit | variable-length slot | fixed-size chunk | extent (start, len) |
| id → data | computed (`id mod N`) + scan | i-node table → chunk chain | i-node table → extent list |
| Free-space accounting | per-bucket append cursor | chunk bitmap | free-extent search |
| Max payload | one sector | unbounded (chain) | unbounded (multi-extent) |
| Fragmentation | garbage slots until compact | internal ~½ chunk/blob | external splinters |
| GC | per-bucket compaction | still needed (below) | compaction with extent moves |
| Suits | many small blobs (L2 nodes) | mixed sizes | few large blobs, streaming |

**Why fixed-size chunks don't win for v1:** they buy O(1) bitmap accounting
but *not* in-place update — flash erases at block granularity, so a freed
bitmap bit can't be reused without erasing its whole block; a FAT-like
allocator ends up log-structured at block level anyway. And they cost
internal fragmentation: with 64 B chunks, ~32 B average tail waste × 100 k
blobs ≈ 3 MB of an 8 MB partition. For the dominant workload (small L2
nodes) variable slots waste strictly less; fixed chunks pay off only once
blobs regularly exceed a sector — which v1 forbids. If such an allocator
lands, the pread extension (contract §5.4) becomes the read path.
