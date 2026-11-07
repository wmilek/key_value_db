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
`blob_db_maintain()` pre-erasing empty sectors — the only thing that erases at
all.

Compaction reclaims *space*, never *ids*. A deleted id stays permanently spent:
the id counter is a durable ceiling that never moves backwards (§13.1), so an
id is never re-issued even after every trace of it has been compacted away.
That is what lets the layers above treat a stale id as safely dead rather than
as a pointer that might silently resolve to somebody else's blob.

Three sectors sit outside this scheme: two **master** sectors, each a header
and an append-only log of state records (the id ceiling, the segmented write in
flight), and a **scratch** sector that compaction stages into so a crash
mid-rebuild cannot lose the bucket. The master is the same idea as a bucket
applied to the allocator's own state: an update appends a record and erases
nothing, and a full log rotates into the other sector (§3.1).

### 1.5 Cost summary

Costs achieved by this design (satisfying contract §3):

| Op | Flash reads | Flash writes | RAM (transient) |
|---|---|---|---|
| `mount` | 2 master headers + ~log₂(records per sector) record slots (7 on 4 KB, 11 on 64 KB) + root bucket walk; + 2045 (bucket scan) with the ceiling in doubt¹ | 0; 1 record or 1 rotation to end a doubt (or format if first ever) | O(1) |
| `alloc_id()` | 0 | 0 (a master record every ~257 ids, unless another record carried the ceiling) | O(1) |
| `get(id)` | 1 bucket header + 12 B per slot + 1 payload | 0 | O(1) |
| `update(id, …)` | 1 bucket header + 12 B per slot² | 1 (slot; on rebind the old slot becomes garbage) + occasional compact | O(1) |
| `delete(id)` | as `get` (verify presence) | 1 (tombstone) | O(1) |
| `exists(id)` | as `get` | 0 | O(1) |
| `count` / `iterate` | 2045 (full scan) | 0 | O(1) |
| `compact_bucket` | 1 | 1 scratch + 1 bucket restore; no master write | O(1) |

¹ With nothing in doubt, mount trusts the newest master record's id ceiling and
reads no other bucket. With the ceiling in doubt (§5.1) it scans all buckets
for the max id — up to ~8 MB of reads, ≈100–200 ms on typical NOR (each slot's
header is read because its `val_len` is needed to skip to the next slot) — and
then writes once, so the next mount does not scan again.
² Release builds perform no existence check — `update` on a dead id is UB
(contract §2). A debug build may spend one bucket read to verify and assert
(§13.5).

## 2. Storage layout

Partition: 8 MB = 2048 × 4 KB sectors (`native_sim` overlay). One sector =
one erase block ("PEB"); on other flash, geometry comes from
`flash_area_get_sectors`.

```
sector  0       master A          ← header + record log (§3.1)
sector  1       master B          ← the other master sector, rotated into
sector  2       scratch           ← shared compaction scratch
sectors 3..2047 bucket 0..2044    ← N = 2045 hash buckets, 4 KB each
```

Sequential id allocation round-robins ids across buckets — uniform fill.
Per-bucket mean at 100 k blobs: 49 entries.

## 3. On-flash format

### 3.1 Master sectors (format major 5)

Each master sector is a header written once per erase, followed by an
append-only log of fixed-size records, each a full copy of the mutable state
(`doc/proposals/2026-10-28-master-log.md` §3):

```
offset 0      header (64 B)     written once, right after the erase
offset 128    record[0]         written with the header
offset 128+R  record[1] ...     appended, one per update
              erased to the end of the sector
```

R is the 48 B record rounded up to the write unit. Filler (header end to 128,
record end to R) is `0xff`, reserved fields 0, integers little-endian.

```
/* --- header: frozen prefix (12 B) — this layout never changes ----------- */
magic[4]         = 'B','D','M','S'          /* allocator identity (D1)      */
format_major[1]  = 5
format_minor[1]  = 0                        /* always 0 in major 5; ignored */
hdr_len[2]       = 64
reserved[2]
prefix_crc16[2]  = CRC16-CCITT(0xffff) over the preceding 10 B
/* --- header body --------------------------------------------------------- */
rot_gen[4]          +1 per rotation; the ok sector with the higher one is newer
peb_size[4]         must equal the device's
n_buckets[2]        must equal the device's
rec_size[2]         R; must equal the build's
incompat[4]         features a reader must know (bit 0: LARGE_PAYLOADS)
ro_compat[4]        features a writer must know
compat[4]           features anyone may ignore
suite[1]            0 = unprotected
reserved0[3]
store_uuid[16]      0 unless protected
max_payload_len[2]  CONFIG_BLOB_DB_MAX_PAYLOAD_LEN; at most the build's (raised, below)
write_align[2]      the write unit slots are padded to; must equal the device's
hdr_crc32[4]        CRC32-IEEE over bytes [0, 60)

/* --- record (48 B) -------------------------------------------------------- */
rec_no[4]           its slot index in this sector
reserved0[4]        compaction keeps no state in the master (§6.1)
next_id_hint[8]     the leading id ceiling (§13.1)
seq_hint[8]         0 unless protected
seg_owner[8]        the segmented write in flight; 0 = none (§5.6a)
reserved1[4]        features live in the header only
fuses[4]            opaque, carried forward unchanged; only a format clears it
reserved2[4]
crc32[4]            CRC32-IEEE over LE32(rot_gen) ‖ bytes [0, 44)
```

A record's CRC covers its own header's `rot_gen`, and `rec_no` must equal its
slot, so a record verifies only where it was written: one copied from an
earlier rotation, or moved within the sector, fails as damage.

**Two CRCs in the prefix, and the split is load-bearing.** A *newer* writer
still produces a prefix whose CRC verifies, so an older reader can trust
`format_major` and refuse the store deliberately. Bit rot produces a prefix
whose CRC fails — a different situation, in which the other master may still be
good.

**Updating.** An update appends one record at the log's end and erases nothing.
The written slots are always a prefix followed by erased slots: after a record
write fails, the next update rotates rather than write the next slot, because
the failed slot may still read erased. That invariant is what lets mount find
the end by a binary search instead of reading the sector. A full log, or a
failed write, **rotates**: the other sector is replaced with a header at
`rot_gen + 1`, the session's feature sets, and record[0]. That is one erase per 82 updates on
4 KB sectors, one per 1 362 on 64 KB.

**Format** writes both sectors, B at `rot_gen` 1 and then A at 2, each a header
and a record[0] for the same state. After a format, no master sector reads
erased except inside a rotation, which is what lets mount treat an erased
sector beside an ok one as doubt (§5.1).

**Features** (§4 of the proposal). `incompat`, `ro_compat` and `compat` replace
the old major per feature combination. A slot-flag feature is set at format:
`LARGE_PAYLOADS` owns slot flags `INDEXED` and `SEGMENT`. A build writes those
flags only into a store whose headers carry the bit, so a build without the
feature may mount a store formatted without it. A build refuses a store with an
`incompat` or `ro_compat` bit it does not know, in any ok header.

**Mount classifies each master header:**

| Sector reads as | Class | Mount |
|---|---|---|
| all `0xff` across the header window | erased | virgin — format, if the other is erased too |
| prefix or header CRC fails | corrupt | ignore it; the other master may be good |
| prefix verifies, foreign magic or a major other than 5 | **foreign** | `-ENOTSUP`, touch nothing |
| prefix and header verify | ok | checked against the build and device (below), then a candidate |

Every ok header must carry this build's suite and `hdr_len`, a payload cap no
larger than this build's, the device's `peb_size`, `n_buckets` and write unit,
the build's R, and only feature bits the build knows; otherwise mount refuses
with `-ENOTSUP` and writes nothing. Two ok headers at the same `rot_gen` cannot
be ordered: `-EIO`.

**A smaller payload cap is raised (D21).** Every slot on such a store fits this
build's cap, so this build reads it as it is. What it must not do is write a
slot above the old cap while a sector that a build with the old cap would
accept still stands, or a fallback to that sector would hand the store to a
build that reads the new slot as a torn tail. So mount rotates, as for a
half-done feature upgrade, until both headers carry this build's cap: two
rotations (two erases), one if a cut left the job half done, all before mount
returns and anything else is written. From then on, builds with the old cap
refuse the store.

A foreign sector on *either* slot refuses the whole store: an interrupted
upgrade can leave this format on one slot and a newer one on the other, and
falling back would silently mount a stale view. Majors 1–4, written before the
master log, are foreign too: they are refused, never migrated. When no master
is usable, mount formats only if both read erased; any other state — bit rot,
a power cut while a format wrote its first sector, another backend's substrate
(§13.7) — is refused with `-EIO` and nothing written, and `blob_db_format()` is
the deliberate discard. `BLOB_DB_AUTOFORMAT_ON_CORRUPT` (default `n`) makes
mount do that discard itself: it erases the whole partition, buckets included,
so no surviving blob can meet the reset id counter. Blank masters do not imply
blank buckets: `blob_db_format()` erases from offset 0 up, so a power cut part
way leaves live buckets behind blank masters. Formatting therefore first
invalidates every bucket header still present, then writes the masters; a cut
before that leaves the masters blank and the next mount repeats it.

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

**A foreign slot is not the end of the log.** A slot that is sealed, has a
`val_len` this build can size, fits the sector and passes its CRC, but carries a
flag outside `BLOB_DB_SLOT_F_KNOWN`, was written by a build with more slot
flags. The walk stops there, because it cannot know what the slot means, but it
reports the slot as foreign, not as a torn tail. The bucket then answers no
lookup (`-EIO`), since a slot past the stop may supersede the one a lookup would
find, and takes no write (`-ENOTSUP`). It is never compacted, so the slot
survives for the build that can read it. If the root's bucket is foreign,
mount refuses with `-ENOTSUP` rather than bind an empty root over it. The CRC
check streams the slot in 64 B reads and runs only where a walk stops at a
sealed slot with an unknown flag.

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

These flags change what a payload means — a reader without the feature would
read an index record as though it were the object's payload — so they belong
to the `incompat` feature `LARGE_PAYLOADS` (§3.1). A build with the feature
sets it at format; a build without it refuses a store that has it, and a build
with it writes no segment into a store that lacks it.

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
classify both master headers                              # §3.1
foreign, or an ok header the build or device does not match -> -ENOTSUP
both ok at the same rot_gen                               -> -EIO
no ok sector:
    both erased: invalidate every bucket header; format both masters; bind root
    elif AUTOFORMAT_ON_CORRUPT: erase everything; format; bind root
    else: return -EIO
read the higher ok sector's log: end = past the last non-erased slot,
    found by a binary search on "fully erased" (the written slots are a prefix),
    newest = the newest record below end that verifies, walking back from end
no record verifies there: read the lower ok sector instead   (fell back)
no record verifies anywhere: no usable master, as above
next_id = newest.next_id_hint                             # §13.1
doubt = the slot just below end fails
     or fell back
     or the other sector is not ok (corrupt, or erased)
     or the store reports damage on a master sector       # UBI
if scratch is sealed: restore the bucket its image names; erase scratch  # §6.1
if doubt:
    max_id_seen = next_id - 1
    for bid in [0..N): walk slots: max_id_seen = max(max_id_seen, slot.id)
    next_id = max(next_id, max_id_seen + 1)               # RAM only
if the ok headers disagree on features,                  # a half-done upgrade
   or an ok header's cap is below the build's: rotate (up to twice)   # D21
if doubt and no rotation yet:
    other sector bad, fell back, or damage: rotate        # rewrites the other sector
    else: append one record                               # the last record verifies again
```

Every record carries the highest ceiling saved so far, and `alloc_id` saves a
ceiling before handing out any id at or above it, so the newest record that
verifies covers every id on flash unless a newer record may have been lost:
torn or rotten as the last one written, in the sector mount fell back past,
or in an other sector that is no longer ok. In each case mount scans.

An **erased** other sector is doubt too. After a format both sectors are
written, so a blank one beside an ok one is only ever a rotation torn after its
erase. That matters for the rotation that ends a doubt. Suppose the newer
sector's header rots: mount takes the older one, doubts, scans and rotates into
the rotten sector. If power is cut after that erase, the next mount sees the
older sector beside an erased one. Without this rule it would take the older
ceiling and hand out the ids bound above it a second time.

**The doubt ends before mount returns.** The scan raises `next_id` in RAM; the
write after it is what tells the next mount the ceiling is safe, so it comes
after the scan, never before. When the doubt came from the other sector, the
write is a rotation into it; when only the last record failed, one appended
record. A cut during that write leaves doubt again, and the next mount scans
and writes again; the scan itself writes nothing. Without this, the doubt would
last until the next rotation, which on UBI with atomic replace can be never.

A store that keeps old copies of a block can hide a lost record. Under UBI, a
newest master that fails its data seal at attach is replaced by its previous
copy if reclaim has not taken it yet — a whole rotation older, valid, beside a
valid other one. UBI reports the damage it acted on, and the backend passes it
on through `blob_db_store_open_found_damage()`; damage on either master sector,
or damage UBI could not attribute to a block, is doubt too.

### 5.2 `alloc_id`

```
if next_id >= next_id_hint: append a master record     # raises the ceiling, §13.1
return next_id++
```

Every master record raises the ceiling to `next_id + 1 + STEP` (256), so a
record written for any other reason — a segmented write's `seg_owner`, a
mount ending its doubt — carries the ceiling forward, and allocation needs a
record of its own only when none was written within the step.

### 5.3 `update` — first bind and rebind (same append path)

```
bid = id % N
ensure bucket formatted (header write on first use)
if write_cursor[bid] + slot_size(len) > sector_end:
    compact_bucket(bid); if still no room: return -ENOSPC
build slot in the slot staging buffer; write it at the write cursor
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

### 5.4a Optional slot index (`CONFIG_BLOB_DB_SLOT_INDEX`)

**The walk is the cost.** A `get` is one 16 B bucket-header read, one 12 B read
per slot in the bucket, then the payload and a 2 B CRC. The walk cannot stop at
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
| `get` (hit) | 1 + slots + 2 reads | 3 reads |
| `update` (cursor known) | 1 + slots reads, then the append | the append |
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
further ids are not indexed and their lookups walk. Nothing is evicted, unless
the table is a cache (below).
`CONFIG_BLOB_DB_SLOT_INDEX_VERIFY` makes every indexed answer also walk and
assert agreement. The `.slot_index` test scenarios run the blob_db, containers,
kvdb and blobfs suites that way, on both backends, with large payloads, and
with a 64-cell table.

**As a cache (`CONFIG_BLOB_DB_SLOT_INDEX_CLOCK`).** Sized for every live id,
the table can need hundreds of KB. Without eviction it fills with whatever comes
first, and stays that way. Every append makes an entry, and a compaction
re-learns all of a bucket's survivors in on-flash order. So ids that are written
and never read (log records, say) can hold the room that the ids read on every
operation need. With the option the table is a cache of what is looked up:

- an entry is made only when a walk finds an id (`learn`);
- an append, or a compaction's re-index, only refreshes an entry already
  present (`blob_db_sidx_rebuild_begin/_end` keep the bucket's entries and drop
  those whose id did not survive);
- a full table evicts by CLOCK: a reference bit per cell, set on each hit, and
  a hand that clears set bits and evicts the first cell it finds clear. A new
  entry starts with its bit set, so it survives the hand's next pass.

Eviction only removes entries, so the rules above still hold. RAM is 2 bits per
cell more. On a recorded rewrite-heavy soak (one two-level map, 1.96 M gets,
UBI on 64 KiB blocks, about 12 k live ids), 128 cells (1.5 KB) read 20.7 M
times, against 19.1 M for a 32 768-cell table (384 KB) holding everything and
100.3 M for a 4 096-cell one without eviction. The hot set was a few hundred
ids: the map's directories and buckets. The `.slot_index.clock` scenarios run
the blob_db and containers suites on a 16-cell cache with every answer
cross-checked. `src/slot_index_clock.c` checks that hot ids stay indexed
through cold traffic and compactions.

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

Read the bucket into the sector buffer and compact it there, in place: each
live slot moves down over the bytes the walk has already passed, so one
sector-sized buffer holds both the old image (ahead of the walk) and the new
one (behind it). Check that what it drops past the end of its walk is only
what one torn append can leave (§6.2), then:

```
erase scratch; write new image to scratch; seal it   ── enters the window
erase bucket;  write new image to bucket
erase scratch                                        ── leaves the window
```

No master is written. The sealed scratch is what marks the window, and mount
restores the bucket from it whatever the master says (§6.1). On a backend that
replaces a block atomically (UBI with `CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE`), the
bucket is replaced in one call and scratch is not used.

### 5.6a Segmented `update` / `delete`, and the sweep

```
update(id, payload, len) with len > MAX_PAYLOAD_LEN:
  0. capture the outgoing index's seg_id[] (if the id holds one)
  1. master record: seg_owner = id               ── enters the sweep window
  2. for j in 0..K-1: alloc an internal id, append a SEGMENT slot
  3. append the INDEXED slot at id                ── ATOMIC COMMIT
  4. tombstone the outgoing segments
  5. master record: seg_owner = 0                ── leaves the sweep window

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
  1. master record: seg_owner = id
  2. new table := old table
  3. for each segment the range touches, or beyond the old count:
       materialise the chunk (old bytes, caller's bytes overlaid, zeros past
       the old end) and append it under a FRESH id; new table[j] = that id
  4. append the INDEXED slot                      ── ATOMIC COMMIT
  5. tombstone only the ids where old table != new table
  6. master record: seg_owner = 0
```

Steps 1 and 5 (1 and 6) are one master record each: a write of R bytes, no
erase.

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

Mount runs the sweep only when the adopted record carries a non-zero `seg_owner`, so the
clean path pays nothing. The sweep walks every bucket by slot header, and for
each live SEGMENT slot owned by that id checks the owner's index — O(N) sector
reads, O(1) RAM beyond the segment tables, idempotent, and restartable after a
crash during the sweep itself.

Every append above, the segment, index and segment-tombstone slots included,
takes its cursor through `confirm_cursor()`, as the inline writers do (§6.2).
A log that ends at a programmed but unparseable header reads as full, so the
writer compacts before appending instead of programming over the torn bytes.
Releasing a segment compacts a bucket with no room rather than skipping it:
the caller clears `seg_owner` once the release returns, so a skipped segment
would not be left for a sweep, it would leak.

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
Like the rest of the API, each call takes the blob_db lock, so the shell may
run them while the application uses the store. A command that reads several
buckets sees each as it is then, not one snapshot.

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
slot append / master record is the same single-op commit as the scalar path,
so a crash mid-batch leaves exactly the completed elements (contract §2).

## 6. Atomicity & crash recovery

Two atomic-commit primitives: a **single slot append** (torn → CRC fails →
treated as end-of-log) and a **master record** (torn → the previous record
stands, and mount doubts). Compaction uses neither: its window is marked by the
scratch seal alone.

### 6.0 Master crash table

| Crash during | Mount sees | Outcome |
|---|---|---|
| a record append | the last record torn, or the slot still erased | the previous record stands; the update was never acknowledged. A torn record is doubt: scan, then one append |
| a rotation, before the header lands | the other sector erased or corrupt; the full sector ok | the full sector's newest record stands; doubt (an erased sector counts), scan, rotate |
| a rotation, after the header, during record[0] | a header with no record that verifies | that sector is unusable; fall back to the other; doubt, scan, rotate into it |
| the write that ends a doubt | a torn record or a torn rotation | doubt again: the next mount scans and writes again |
| a format, between its two sector writes | B ok, A erased | doubt; a scan of buckets the format has emptied; rotate into A |
| a format, inside B's write | no usable master, A erased | `-EIO`, nothing written; `blob_db_format()` starts again |
| a `seg_owner` set / clear record | the old value | as `BLOB_DB_CUT_AFTER_OWNER_SET` / `_AFTER_RELEASE`: an unset owner means no segment was written yet, a still-set one means the sweep runs again |
| `erase_all`'s record | the old record, or a torn one | the old, higher ceiling stands, or doubt and a scan; never a ceiling below an id on flash |

A record write that fails without a power cut may leave its slot programmed,
partly programmed or erased. The next update rotates instead of writing the
next slot, so no erased slot ever lies below a written one.

### 6.1 Compaction crash table

| Crash point | Mount sees | Recovery |
|---|---|---|
| before any write | scratch erased | nothing |
| during scratch erase/write | scratch **unsealed** | nothing; the bucket was never touched, and the next compaction erases scratch first |
| during the scratch seal write | seal partial → unsealed | as above |
| during bucket erase/restore | scratch **sealed** | copy scratch → bucket; erase scratch |
| during the final scratch erase | sealed, or partly erased → unsealed | sealed: restore the same bytes again (idempotent). Unsealed: the bucket is already complete |

**The seal decides, and the master holds no compaction state.** Mount checks
the seal at every mount, on every backend. Scratch is sealed only after the
whole image landed and erased before the window closes, so a sealed scratch
exists exactly while the bucket may be erased or half written, and its image is
the bucket's complete new content. The bucket comes from the image's own
header, which the seal covers. Atomic-replace builds check too, because a store
compacted through scratch by a build without atomic replace can carry a sealed
scratch into one with it. A mount with nothing to recover reads the 16 B seal
and nothing else. Recovery writes no master.

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
the next compaction. A torn slot whose header can still size it is stepped over
like any rotten slot (§3.4). One torn inside its header ends the log on
programmed bytes, and the next append to that bucket would be programmed over
them and fail its own CRC. So every writer treats such a bucket as full and
compacts it first (`confirm_cursor()`).

**What compaction may drop.** Compaction keeps what its walk reaches and drops
the rest, so it first checks that the rest is only what a torn append leaves.
One append programs one slot, at most `slot_size_for(MAX_PAYLOAD_LEN)` bytes
from where the walk stopped. Within that span its bytes may be programmed in any
order: a NOR page program cut midway can leave bits anywhere in the page, beyond
a header torn too badly to size. A programmed byte further out is data the walk
could not reach (a build with a larger payload cap, or a stride the walk lost),
so compaction refuses with `-EIO` and drops nothing. A foreign slot at the stop
refuses it with `-ENOTSUP` (§3.4).

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
| Crash mid-compaction | the scratch seal (§6.1); ids unaffected |
| Crash mid-master record or rotation | the previous record stands; mount doubts, scans and writes once (§6.0) |
| Bit corruption in a slot | CRC catches it; **only that slot** is unreachable — the header-driven walk steps over it (§3.4) and the next compaction drops it. Readers fall back to the newest intact slot for the id |
| Partition full | `update` returns `-ENOSPC` after attempting compaction of the target bucket |
| One bucket overflows | as above, per-bucket; round-robin allocation keeps fill uniform, so buckets fill together |
| Id space exhaustion | 2⁶⁴ allocations ≈ 5800 years at 100 M/s — not reachable |
| Crash during a segmented write | one rule — an unlisted segment is garbage — covers all three points; mount sweeps when the adopted record carries `seg_owner` (§5.6a) |
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
13. simulated mid-compaction crash recovers from the scratch seal alone,
    whatever the master holds, with no master write
16. master log (`src/master_log.c`): the format's image of both sectors;
    records erase nothing; rotation; a failed record write rotates; geometry,
    suite and unknown feature bits refused in either sector; mount's doubt
    rules and the one write that ends each; power cuts at every byte of a
    record append and of a rotation (`src/power_cut.c`); the whole suite with a
    3-record log (`*.master_log_tiny`)
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
   `next_id_hint` in a master record is a **leading ceiling**: an exclusive
   upper bound on every id ever returned by `alloc_id`. Every record raises it
   to `BLOB_DB_ID_HINT_STEP` (256) past the next id, `alloc_id` appends a
   record when it reaches the ceiling and no other record has carried it, and
   mount takes `next_id = next_id_hint` (the bucket scan, run only when the
   ceiling is in doubt, may *raise* it, never lower it — §5.1). The one record
   that lowers it is `erase_all`'s, written after every bucket is invalidated,
   to one step past id 2. This protects
   allocated-but-unbound ids across a crash — the property the model
   container's intent recovery and (future) root-registry `get_or_create`
   depend on. It subsumes both earlier monotonicity patches (tombstones toward
   the scan max, the `hint↑` compaction ride-along). Cost: at most one master
   record, no erase, per block of 256 allocations. Note: the ceiling never lowers, so `next_id`
   never recovers to a previously issued value after a crash — which is why
   the root convention is realized as a *reserved* id (contract D7): format
   itself consumes and binds id = 1, so no client ever depends on the counter
   recovering to exactly 1, and virgin re-bootstrap is detected by id 1's
   empty payload (rootreg §6), not by re-allocating it.
2. **Steady-state RAM story.** *(Resolved: re-scan.)* The implementation
   keeps **no** `write_cursor[N]` array; each write re-walks the target
   bucket (`walk_bucket`) to find the append cursor (+1 read on
   `update`/`delete`), honoring contract R1 (O(1) steady-state RAM).
3. **Sector-size portability.** *(Resolved.)* The one sector buffer is sized
   by `BLOB_DB_SECTOR_BUF_SIZE` and mount refuses a partition whose sector is
   larger. Compaction works in place in that buffer (§5.6), so there is no
   second sector image. Slot staging moved off the stack into its own
   `.bss` buffer of `MAX_PAYLOAD_LEN + 14` B rounded up to 32, so the payload
   cap no longer drives stack depth (contract R7). The cap itself
   is checked against the geometry at mount: a bucket is an append-only log,
   so a rebind needs two slots to coexist, bounding the payload at
   `(sector − 16) / 2 − 14` — about 2026 B on 4 KB sectors, 32 746 B on 64 KB.
4. **Master format version field.** *(Resolved.)* Superseded by the frozen
   compatibility prefix (§3.1): `format_major` / `format_minor` / `hdr_len`
   under their own CRC, so software of any vintage can classify a store it did
   not write and refuse it rather than reformat it. Major 5 adds feature sets in
   the header for everything that would once have taken a minor or a major.
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
   on-flash format change: in major 5 it would be a feature bit or a header
   field, not a `format_minor` bump, which major 5 does not use.

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
