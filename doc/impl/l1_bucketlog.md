# Implementation design — bucket-log allocator for `blob_db`

Status: v2 · **Non-normative feasibility design.** This document proves the
L1 contract (`doc/layers/l1_blob_db.md`) is implementable and guides the
first implementation. Everything here — formats, algorithms, costs — may be
fine-tuned during implementation as long as the contract holds. Upper layers
must not depend on anything in this document (P6). Known deltas against the
contract are tracked in §13 *Open implementation items*.

Target board for bring-up: `native_sim`.

---

## 1. The idea, and what it costs

### 1.1 What flash allows

Everything below follows from one property of NOR flash: **programming only
clears bits (1→0), and the only way to set them back is to erase — a whole
sector at a time.** Nothing can be modified in place. A design can therefore
do exactly two things cheaply: *append* into space that is still erased, and
*read*. Erasing is the expensive, destructive operation, so it must be rare
and must never be the only thing standing between a crash and the data.

That rules out any layout that maintains a record by rewriting it. What is
left is a log: write forward, and let the newest entry define the truth.

### 1.2 A bucket is one erase sector

The allocator divides the partition into **buckets, one per erase sector**, and
a blob with id `i` lives in bucket `i mod N`. That is the whole index: the id
*is* the address, so nothing has to be looked up, cached, or rebuilt at mount
to find where a blob lives (P3, P5).

A bucket holds a small fixed **header** and, after it, an append-only stream of
variable-length **slots**:

```
┌────────────┬────────────┬────────────┬───────────┬──────────────────────────┐
│ hdr (16 B) │ slot       │ slot       │ slot      │ still erased (0xff …)    │
└────────────┴────────────┴────────────┴───────────┴──────────────────────────┘
                                                   ▲
                                                   write cursor
```

The header is written once per erase; slots accumulate after it. Each slot is
self-describing — flags, `val_len`, the id, the payload, a CRC — and `val_len`
gives the stride to the next slot, so the stream is walked without any index.
The **write cursor** is not stored anywhere: it is the first offset whose slot
header cannot be trusted (still erased, or malformed), found by walking.

### 1.3 Three operations, one primitive

Every mutation is the same primitive — *append a slot* — and the rule that
gives it meaning is **latest wins**: for a given id, the last matching slot in
append order is the truth.

| Operation | What is written | What happens to the old bytes |
|---|---|---|
| bind (first `update`) | a slot carrying the payload | — |
| rebind (later `update`) | a *new* slot with the new payload | the previous slot is left in place and becomes garbage |
| `delete` | a slot with the TOMBSTONE flag and no payload | likewise |

So a rebind does not overwrite, and a delete does not remove. Both append, and
both work by *shadowing* what came before. A read walks the bucket and takes
the last slot for the id: if that slot is a tombstone, the blob reads as gone
(`-ENOENT`); otherwise its payload is the current value.

```
append order ──►
  [id=7 "aaa"]   [id=7 "bb"]   [id=7 ✝]
   garbage        garbage       current: id 7 is dead
```

This is also what makes a mutation atomic against power loss for free. A slot
is assembled complete in RAM — flags, id, payload, and a CRC over all of it —
and handed to the store as **one write into erased space**. It therefore either
lands whole, or lands as something that fails its own CRC (or never gets its
SEALED flag), which readers skip. Either way the *previous* slot for that id is
untouched and is still the newest valid one, so the old value survives intact.
No journal, no two-phase write, no recovery pass: the commit point *is* the
moment the new slot verifies.

### 1.4 Garbage, and getting rid of it

The cost of never overwriting is that dead slots accumulate: every rebind and
every delete leaves one behind. A bucket therefore fills faster than its live
contents suggest. Compaction is lazy: when an append finds the next slot would
not fit, the allocator **compacts** the bucket first — builds an image holding
only the live slots, dropping tombstones and superseded ones, stages it in the
scratch sector, then erases the bucket and writes the image back (§5.6). This
is where a tombstone's bytes finally disappear, and — apart from `format`, and
`prepare` pre-erasing empty sectors — the only thing that erases at all.

Compaction reclaims *space*, never *ids*. A deleted id stays permanently spent:
the id counter is a durable ceiling that never moves backwards (§13.1), so an
id is never re-issued even after every trace of it has been compacted away.
That is what lets the layers above treat a stale id as safely dead rather than
as a pointer that might silently resolve to somebody else's blob.

Two sectors sit outside this scheme: a **master** (double-buffered across two
sectors) carrying the generation counter, compaction state and the id ceiling,
and a **scratch** sector that compaction stages into so a crash mid-rebuild
cannot lose the bucket.

### 1.5 Cost summary

Costs achieved by this design (satisfying contract §3):

| Op | Flash reads | Flash writes | RAM (transient) |
|---|---|---|---|
| `mount` | 2 (master A + B) + root bucket walk; + 2045 (bucket scan) with a master in doubt¹ | 0 (or format if first ever) | O(1) |
| `alloc_id()` | 0 | 0 (occasional id-hint persist) | O(1) |
| `get(id)` | 1 bucket header + 1 per window of slots walked³ + 1 payload and CRC | 0 | O(1) |
| `update(id, …)` | 1 bucket header + 1 per window of slots walked²³ | 1 (slot; on rebind the old slot becomes garbage) + occasional compact | O(1) |
| `delete(id)` | as `get` (verify presence) | 1 (tombstone) | O(1) |
| `exists(id)` | as `get` | 0 | O(1) |
| `count` / `iterate` | 2045 (full scan) | 0 | O(1) |
| `compact_bucket` | 1 + master writes | 1 scratch + 1 bucket restore + 2 master | O(1) |

¹ With both masters valid, mount trusts the newer one's id ceiling and reads
no other bucket. With a master in doubt (§5.1) it scans all buckets for the max
id — up to ~8 MB of reads, ≈100–200 ms on typical NOR (each slot's header
is read because its `val_len` is needed to skip to the next slot).
² Release builds perform no existence check — `update` on a dead id is UB
(contract §2). A debug build may spend one bucket read to verify and assert
(§13.5).
³ The walk reads `CONFIG_BLOB_DB_READ_WINDOW` bytes at a time (256 by
default) and parses the slot heads in them from RAM (§5.4); after a slot larger
than the window it reads the next head alone. The payload and its CRC are one
read when they fit in a window, two otherwise.

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
| all `0xff` across the header window | erased | virgin — format, if the other is erased too |
| prefix CRC fails | corrupt | ignore it; the other master may be good |
| prefix verifies, foreign magic or unknown major | **foreign** | `-ENOTSUP`, touch nothing |
| prefix and body verify | ok | candidate; highest generation wins |

A foreign sector on *either* slot refuses the whole store, before generation
is considered: an interrupted upgrade can leave the old format on one slot and
a newer one on the other, and falling back to the older would silently mount a
stale view. When neither master is usable, mount formats only if both read
erased; any other pair — bit rot, a power cut while a format wrote the first
master, another backend's substrate (§13.7) — is refused with `-EIO` and
nothing written, and `blob_db_format()` is the deliberate discard.
`BLOB_DB_AUTOFORMAT_ON_CORRUPT` (default `n`) makes mount do that discard
itself: it erases the whole partition, buckets included, so no surviving blob
can meet the reset id counter. Blank
masters do not imply blank buckets: `blob_db_format()` erases from offset 0 up,
so a power cut part way leaves live buckets behind blank masters. Formatting
therefore first invalidates every bucket header still present, then writes the
master; a cut before that write leaves the masters blank and the next mount
repeats it.

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
read master A, B
if neither valid:
    if both erased: invalidate every bucket header; write master; bind root
    elif AUTOFORMAT_ON_CORRUPT: erase everything; write master; bind root
    else: return -EIO                                     # §3.1
else:
    pick higher valid generation
    if state == COMPACTING: finish/abort per crash table (§6.1), write CLEAN master
next_id = next_id_hint                                    # §13.1

if exactly one master valid, or the store reports damage on a master sector:
    max_id_seen = next_id - 1
    for bid in [0..N):
        walk slots: max_id_seen = max(max_id_seen, slot.id)   # tombstones count too
    if max_id_seen >= next_id: next_id = max_id_seen + 1; persist a new ceiling
```

Every master write carries the highest ceiling saved so far, and `alloc_id`
saves a ceiling before handing out any id below it, so when both masters are
valid the newer one's ceiling already exceeds every id on flash, and mount
trusts it. With only one valid master the newer may be the one lost — to bit
rot, or to a torn write of a ceiling this scan raised — and the two cannot be
told apart, so mount scans. That also covers a store formatted and not yet
written since, whose second master is still blank; its buckets are nearly all
unformatted, one header read each.

A store that keeps old copies of a block can hide that loss. Under UBI, a
newest master that fails its data seal at attach is replaced by its previous
copy if reclaim has not taken it yet — a valid master with an older ceiling,
beside a valid other one, which the generations cannot tell from a healthy
pair. UBI reports the damage it acted on, and the backend passes it on through
`blob_db_store_open_found_damage()`; damage on either master sector, or damage
UBI could not attribute to a block, makes mount scan too.

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
answering one lookup reads the slot heads, not one erase block, which on 64 KB
sectors is the difference between ~12 B per slot and 64 KB.

The heads are read `CONFIG_BLOB_DB_READ_WINDOW` bytes at a time (256 by
default) into a buffer on the walker's stack and parsed from there, not one
12 B read each. A read costs mostly its fixed overhead (§5.4a: about 69 µs on
the DK's QSPI NOR against 0.25 µs per byte; on UBI also a lock and a lookup),
so a window pays for itself as soon as it holds two heads. After a slot larger
than the window (a segment) the next refill reads just the 12 B head, since
the window would hold nothing else. The buffer lives for one walk: it holds
bucket bytes, and keeping them between calls would be a data cache (P3).
The payload and its 2 B CRC are read together when they fit in a window.

`count` / `iterate` and compaction keep working on a resident sector image:
their liveness test is "does a later slot share this id?", which is O(n²) in
slots, and turning each comparison into a flash read would cost far more than
the sector read it replaced. They are diagnostics and maintenance, not the hot
path (contract D5).

### 5.4a Optional slot index (`CONFIG_BLOB_DB_SLOT_INDEX`)

**The walk is the cost.** A `get` was one 16 B bucket-header read, one 12 B read
per slot in the bucket, then the payload and a 2 B CRC; the figures in this
paragraph were measured that way, before the walk read a window at a time
(§5.4). The walk cannot stop at
the first match, because the newest slot wins and the log has no back-pointers.
On the nRF5340-DK's QSPI NOR a read costs about 69 µs of command overhead plus
0.25 µs per byte, so a 12 B header read is 72 µs, almost all of it overhead. In
`app_cbor_persondb` at 8 000 persons the walk is **73 %** of an access check's
flash time at the shipped kvhash geometry (18 slots per walk) and **91–95 %**
once kvhash buckets are made smaller (51–102 slots per walk). About half of the
slots walked are superseded versions awaiting compaction. Measured on
`native_sim` with every flash read classified, timed on the DK flash model
(`app_perf_l0/models/mx25r64_nrf5340dk_full.json`).

**The index.** An opt-in, fixed-size RAM table from id to the offset of that
id's newest slot, and one append cursor per bucket
(`lib/blob_db/blob_db_slot_index.c`). With a hit, a `get` is three reads: slot
head, payload, CRC. The bucket header read is skipped too, because the index
holds a cursor only for a bucket whose header it has seen valid. `update` takes
its append cursor from the index instead of walking.

| Operation | Without | With the index |
|---|---|---|
| `get` (hit) | 1 + windows walked + 1 reads (was 1 + slots + 2) | 2 reads: slot head, then payload and CRC (was 3) |
| `update` (cursor known) | 1 + windows walked reads, then the append | the append |
| `mount` | no scan with both masters valid (§5.1) | the same; a scanning mount also fills the index, otherwise each bucket's first walk does |
| RAM | none | 12 B per table cell + 6 B per bucket, fixed by Kconfig |

**Why it can be trusted.** The index is a hint, and it is built so that the one
error no check could catch, naming an *older* slot of an id, cannot arise:

- Per bucket, the cursor is either unknown or the true end of the log, and an
  unknown cursor means the table holds nothing for that bucket.
- Every slot append goes through `append_slot2()`, which updates the entry and
  advances the cursor. An append anywhere but at the known cursor, or a failed
  write that may have left a torn slot, makes the index forget the bucket.
- Anything that rewrites a bucket other than by appending forgets it or rebuilds
  it: `format_bucket()` starts it empty, compaction re-indexes the image it just
  wrote (still in RAM), and `erase_all`, `format`, mount and unmount reset the
  whole table. Mount-time compaction recovery runs before any scan that fills
  the index.
- A hit is still checked against flash: the slot must be sealed, carry the same
  id, fit below the cursor and pass its CRC. Anything else, and every miss,
  takes the walk exactly as before. A walk over the full log teaches the index
  the answer it found. One that had to fall back past a rotten slot does not.
- Before any append at a cursor the index supplied, the writer reads the 12
  bytes a slot head would occupy there and requires them erased
  (`confirm_cursor()`). If they are not, the index forgets the bucket and the
  writer walks for the true end of log. While mounted, only blob_db writes the
  store, so this should never fire. It is there because programming over
  non-erased flash is the one failure an index must not be able to cause. It
  costs one read per write, and reads do not pay it. The test suite forges a
  torn slot into raw flash under a mounted store, which reaches exactly this
  case.

So a crash, a torn write or bit-rot costs a walk, never a wrong answer, and the
index has no on-flash state and no format impact. When the table is full,
further ids are not indexed and their lookups walk. Nothing is evicted.
`CONFIG_BLOB_DB_SLOT_INDEX_VERIFY` makes every indexed answer also walk and
assert agreement. The `.slot_index` test scenarios run the blob_db, containers,
kvdb and blobfs suites that way, on both backends, with large payloads, and
with a 64-cell table.

**Effect** (`app_cbor_persondb`, 8 000 persons, DK flash model, flash time only):

| kvhash bucket floor | index | check | insert (fill avg) | rewrite | index RAM |
|---|---|--:|--:|--:|--:|
| 4096 (shipped) | off | 10.9 ms | 240 ms | 535 ms | — |
| 4096 | on, 2 048 cells | 3.0 ms | 192 ms | 512 ms | 24 KiB |
| 1024 | off | 24.4 ms | 168 ms | 266 ms | — |
| 1024 | on, 4 096 cells | 2.2 ms | 88 ms | 204 ms | 48 KiB |
| 256 | off | 46.1 ms | 191 ms | 204 ms | — |
| 256 | on, 8 192 cells | 2.1 ms | 56 ms | 77 ms | 96 KiB |

Without the index, smaller kvhash buckets trade read speed for write speed,
because every extra blob lengthens the walk for every lookup sharing its
bucket. With it, blob count stops affecting lookups, and smaller buckets
improve both reads and writes.

**The departure from P3 is deliberate**, and it is why the index is off by
default. Its RAM is set at build time and does not grow with the store, mount
reads nothing extra to fill it, and losing it costs speed, never correctness.
The cell could be made smaller, for example 8 B by storing `id / n_buckets` in
32 bits and the offset in 16 bits where sectors allow; that is not done yet.

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

**Partial write** (`blob_db_write`) reuses the same window and the same commit,
rewriting only the segments the range covers:

```
pwrite(id, offset, buf, len) on a segmented object:
  new_total = max(total_len, offset + len);  new_K = ceil(new_total / seg_len)
  1. master: seg_owner = id
  2. new table := old table
  3. for each segment the range touches, or beyond the old count:
       materialise the chunk (old bytes, caller's bytes overlaid, zeros past
       the old end) and append it under a FRESH id; new table[j] = that id
  4. append the INDEXED slot                      ── ATOMIC COMMIT
  5. tombstone only the ids where old table != new table
  6. master: seg_owner = 0
```

`seg_len` is inherited from the existing index and never recomputed: the
surviving segments are already cut at that stride, so changing it would
misplace every byte they hold. Untouched segments keep their ids and their
slots, which is the whole saving — a four-byte write replaces one segment, not
K. Writing past the end extends sparsely, because a read of the gap between the
old end and the new bytes is served as zeros.

Growing an *inline* payload past one slot returns `-EFBIG` rather than promoting
it to segmented in place. Such a payload still fits the caller's RAM, so
`get` + `update` promotes it, and keeping that out of the library avoids a
second whole-object staging buffer inside it.

Mount runs the sweep only when the master carries a non-zero `seg_owner`, so the
clean path pays nothing. The sweep walks every bucket by slot header, and for
each live SEGMENT slot owned by that id checks the owner's index — O(N) sector
reads, O(1) RAM beyond the segment tables, idempotent, and restartable after a
crash during the sweep itself.

### 5.7 `iterate` / `count`

Bucket-by-bucket full scan; callback runs against the resident sector buffer.
Order is not id-sorted.

### 5.7a Inspection (`CONFIG_BLOB_DB_INSPECT`, `CONFIG_BLOB_DB_SHELL`)

Kept out of the contract: `<app/lib/blob_db.h>` carries only what every
implementation guarantees, and this lives in its own header,
`<app/lib/blob_db_inspect.h>`, and its own source, `blob_db_inspect.c`, which
reaches the core through a few internal hooks in `blob_db_internal.h`.

`blob_db_inspect_info_get()` returns the geometry and master state from RAM.
`blob_db_inspect_bucket_get()` reads one bucket sector and classifies every
slot the way compaction would (§5.6) — *live* if it is the newest for its id and not a
tombstone, otherwise *garbage* — and splits the slot stream into

```
live + garbage + torn + free == sector − bucket header
```

where *torn* is anything programmed past the readable log (a torn append, or a
slot whose CRC failed and what follows it in the resident walk) and *free* is
the erased remainder. A bucket without a valid header counts as entirely free:
the next write formats it.

`CONFIG_BLOB_DB_SHELL` puts a read-only `blob_db` command group over both:

```
blob_db info                      geometry, format, master, id counter
blob_db stats [top_n]             whole store: bytes by class, ids per bucket,
                                  fill histogram, fullest buckets
blob_db buckets [first [count]]   per-bucket table (never-used buckets skipped)
blob_db bucket <bid>              every slot of one bucket, oldest first
blob_db id <id>                   its bucket, size, and its slot history
blob_db dump <id> [max_bytes]     hex dump of the payload
blob_db iostats [reset]           flash I/O counters (CONFIG_BLOB_DB_IOSTATS)
```

These are diagnostics on the implementation, not part of the contract (P6).
Like the rest of the API they are unsynchronised: the shell runs on its own
thread, so use them while the application is not calling blob_db.

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
BLOB_DB_AUTOFORMAT_ON_CORRUPT  bool, default n   # erase on unusable masters (§3.1)
BLOB_DB_LARGE_PAYLOADS         bool, default n   # segmented objects (§3.5)
BLOB_DB_MAX_OBJECT_LEN         int, default 131072   # validated at mount
BLOB_DB_MAX_SEGMENTS           int, default 128      # 16 B of .bss each
BLOB_DB_SEGMENT_LEN            int, default 0        # 0 = sector/4, clamped
BLOB_DB_MULTI                  bool, default n   # batch ops (§5.8, contract D6)
BLOB_DB_INSPECT                bool, default n   # blob_db_inspect.h (§5.7a)
BLOB_DB_SHELL                  bool, depends on SHELL, select INSPECT
module = BLOB_DB (standard LOG pattern)
```

Partition geometry discovered at runtime (`flash_area_get_size/_get_sectors`).

## 8. Repo integration

```
lib/blob_db/{blob_db.c, alloc_bucketlog.c, blob_db_internal.h, Kconfig, CMakeLists.txt}
include/app/lib/blob_db.h          the L1 contract — nothing else
include/app/lib/blob_db_inspect.h  bucket-log diagnostics (§5.7a)
include/app/lib/blob_db_iostats.h  flash I/O counters (CONFIG_BLOB_DB_IOSTATS)
include/app/lib/blob_db_test.h     crash-test hooks (CONFIG_BLOB_DB_TEST_CRASH_HOOKS)
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
   one is reached, and mount takes `next_id = next_id_hint` (the bucket scan,
   run only when a master is in doubt, may *raise* it, never lower it —
   §5.1). This protects
   allocated-but-unbound ids across a crash — the property the model
   container's intent recovery and (future) root-registry `get_or_create`
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
7. **A foreign *substrate* classifies as `CORRUPT`, not `FOREIGN`.** The
   compatibility prefix (§3.1) lets a build classify a store written by
   another *allocator* and refuse it (D1). It does not reach the *backend*
   axis added by the `blob_db_store` seam: booting a `flash_area` build on a
   partition holding a UBI volume fails the prefix CRC before the magic is
   ever compared, so both masters land in `CORRUPT` and mount refuses with
   `-EIO` — nothing is written, but the error reads as damage rather than as
   a store of the other kind. (`BLOB_DB_AUTOFORMAT_ON_CORRUPT=y` erases it
   instead; the option used to default to `y`.) Measured, both
   directions, in `doc/impl/l0_backends.md` §4; the reverse direction refuses
   with `-ENOTSUP` because UBI's own attach refuses first. Candidate fix: a
   backend-id byte inside the frozen prefix so a substrate mismatch
   classifies as `FOREIGN` and is refused with `-ENOTSUP`. That is an
   on-flash format change — cheap now, since the prefix already carries
   `hdr_len` and reserved space, and it needs a `format_minor` bump at
   most.

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
