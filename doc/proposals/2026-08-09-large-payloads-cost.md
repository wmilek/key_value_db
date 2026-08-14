# Addendum — what large payloads cost, under a filesystem workload

Status: **analysis / for decision** · 2026-08-09
· Companion to `doc/proposals/2026-08-09-large-payloads.md`
· Premise change: `blobfs` implemented **directly on `blob_db`**, not on an
  L2 `seq` chunk chain (`doc/layers/l3_interfaces.md` §4)

The base proposal asked *can* payloads be large. This one asks *what it
costs*, and whether a constrained device should be able to opt out and keep
today's small-payload implementation.

**Headline.** The feature itself is cheap: **+1.8 KB `.text`** and
**16 bytes of `.bss` per supported segment** — measured, not estimated
(§1). What is *not* cheap is the workload the filesystem brings: on 64 KB
sector QSPI NOR the bucket-log reads a whole 64 KB sector to extract one
chunk, so a sequential file read costs **8–16× read amplification** (§3).
That cost is not caused by segmentation — it is already in the current
implementation, and it is also what makes `blob_db`'s `.bss` 128 KB on that
board. **The opt-out question is therefore the wrong knob**: keeping
small-payload-only saves ~2 KB of flash and ~2 KB of RAM, while the whole-
sector buffering it leaves in place costs 128 KB (§5). See the correction in
§3 on how much of that 128 KB the streaming walk actually recovers — less than
this document first claimed.

---

## 1. Measured cost of the feature

**Method.** `gcc -Os -ffunction-sections`, sections summed per object, host
x86-64 with stubbed Zephyr headers (`kernel.h`, `log.h`, `crc.h`) — no ARM
cross-compiler in this environment. Absolute figures are therefore
*indicative*; Thumb-2 at `-Os` is typically 0.8–1.0× x86-64 for
pointer-and-branch code like this, so treat the **ratios** as the robust
result. The measured subject is a working sketch of §6 of the base proposal:
`index_load`, `seg_fetch`, `blob_db_size`, `blob_db_read`, the segmented
write path with master intent, segmented delete, and the orphan sweep.

| Object | `.text` | `.rodata` | `.bss` |
|---|---|---|---|
| `blob_db.c` today (`MAX_PAYLOAD=256`, 4 KB sectors) | 6 687 | 1 540 | 8 256 |
| `blob_db.c` today (`MAX_PAYLOAD=4066`) | 6 696 | 1 540 | 8 256 |
| **segmentation layer** (K ≤ 251) | **1 883** | 46 | **4 016** |
| dispatch changes inside `blob_db.c` (flag tests in `get`/`update`/`delete`/`for_each_live_slot`, scatter `append2`, master `seg_owner`) | ~300–500 *(est.)* | — | +8 |

So Stage 2 is **+2.3 KB `.text`, +34 % on `blob_db`'s code size.**

Two results worth noting:

- **`.text` is independent of the maximum payload size** — 1 883 B at every
  `MAX_SEGMENTS` setting from 33 to 512. The algorithm does not grow with the
  blob.
- **Raising `MAX_PAYLOAD_LEN` from 256 to 4066 costs 9 bytes of `.text`
  today** — and the *entire* difference is in `append_slot`. The current
  Kconfig cap is buying nothing; it is only bounding the stack frame that
  Stage 1 removes anyway.

### 1.1 RAM scales with segment *count*, not payload size

The segmentation layer's `.bss` is two segment-id tables, `8 × K` bytes each,
measured exactly linear:

| `MAX_SEGMENTS` | 33 | 131 | 251 | 512 |
|---|---|---|---|---|
| `.bss` | 528 B | 2 096 B | 4 016 B | 8 192 B |

`K = ceil(max_payload / chunk)`, so **larger erase blocks make large-blob
support cheaper in RAM** — the exact inverse of the "raise the cap" option,
where RAM cost was 2 bytes per payload byte:

| Target max blob | 4 KB sectors (chunk 2014) | 64 KB sectors (chunk 8192) |
|---|---|---|
| 64 KB | K=33 → 528 B | K=8 → 128 B |
| 256 KB | K=131 → 2 096 B | K=32 → 512 B |
| 505 KB | K=251 → 4 016 B | K=63 → 1 008 B |
| 8 MB (whole partition) | not reachable, single-level index | K=1024 → 16 KB |

Against the 8 256 B (4 KB sectors) or 128 KB / 131 136 B (64 KB sectors) that
`blob_db` already holds in `.bss`, this is a rounding error.

Getting there needs one implementation detail the base proposal did not
mention: `append_slot` must gain a **scatter variant** taking
`(header, payload)` as two fragments, so a segment slot assembles directly
into the existing staging buffer. Without it each segment needs a third
sector-sized buffer. The sketch above assumes the scatter form.

---

## 2. Flash overhead and capacity

Per segment: 14 B slot header + 12 B segment header = **26 B**, plus
write-align padding. Per blob: one index slot of `14 + 16 + 8K` bytes.

The binding constraint is not overhead but **bucket packing**. A segment slot
is `26 + chunk` bytes and a bucket's data area is `peb − 16`; chunk sizes that
divide it evenly maximise capacity but leave a full bucket with **zero**
headroom, so at high fill the partition hits `-ENOSPC` abruptly instead of
degrading:

**4 KB sectors** (index cap `K ≤ 251`):

| chunk | segments/bucket | headroom left | max blob | space efficiency |
|---|---|---|---|---|
| 256 B | 14 | 132 B | 63 KB | 87.5 % |
| 512 B | 7 | 314 B | 126 KB | 87.5 % |
| **1024 B** | 3 | **930 B** | **251 KB** | 75.0 % |
| 1334 B | 3 | 0 B | 327 KB | 97.7 % |
| 2014 B | 2 | 0 B | 494 KB | 98.3 % |

**64 KB sectors** (index cap `K ≤ 4091`):

| chunk | segments/bucket | headroom left | max blob | space efficiency |
|---|---|---|---|---|
| 2 KB | 31 | 1 226 B | > partition | 96.9 % |
| **8 KB** | 7 | **7 994 B** | > partition | 87.5 % |
| 32 734 B | 2 | 0 B | > partition | 99.9 % |

Reading this: on 4 KB sectors, "hundreds of kilobytes" and "graceful
behaviour at high fill" pull against each other. 1024 B chunks give 251 KB
per blob with real headroom; reaching ~500 KB means chunks that pack buckets
solid. On 64 KB sectors there is no tension — 8 KB chunks reach the partition
limit with 8 KB of headroom per bucket.

Two placement notes that came out of writing the sketch:

- **`-ENOSPC` on a segment is recoverable.** A segment's id is internal and
  arbitrary, and `bucket = id mod N`, so if the chosen bucket is full the
  writer can simply call `alloc_id()` again and land somewhere else. A
  bounded retry loop turns hash placement into a probe sequence. This does
  *not* help user-visible ids, whose bucket is fixed — but those are small.
- **Prepared buckets need no read.** `blob_db_prepare()` leaves a bucket
  formatted and empty, so its write cursor is known to be 16 without reading
  it. A large sequential write into prepared buckets can skip the
  walk-to-find-cursor read entirely (§3).

---

## 3. Time cost — and where the filesystem actually hurts

Using this repo's own figure for the target board: *"roughly one second per
bucket at 8 MHz Quad-SPI on mx25r64"* for a 64 KB sector erase
(`include/app/lib/blob_db.h`, `blob_db_prepare` docs); ~4 MB/s sequential
read, so a 64 KB bucket read ≈ 16 ms and a 4 KB bucket read ≈ 1 ms.

256 KB file, 64 KB sectors, 8 KB chunks (K = 32):

| Operation | Cost | Comment |
|---|---|---|
| Cold write (buckets never formatted) | 32 erases ≈ **32 s** | why `blob_db_prepare()` becomes mandatory, not optional |
| Warm write | 32 bucket reads (512 ms) + 32 slot writes (~700 ms) ≈ **1.2 s** | ≈ 210 KB/s |
| Warm write into *prepared* buckets | ~700 ms | the 512 ms of reads disappears |
| Sequential read, index cached | 32 × 64 KB = **2 MB read for 256 KB** | **8×** |
| Sequential read, no index cache | 64 × 64 KB = **4 MB** | **16×** |
| Random 4-byte `pwrite` | 16 + 16 + 22 ms ≈ **55 ms**, 8 KB written | **2048×** write amplification |

Same file on 4 KB sectors with 2 KB chunks (K = 131):

| Operation | Cost |
|---|---|
| Sequential read, index cached | 131 × 4 KB = 524 KB for 256 KB → **2×** |
| Random 4-byte `pwrite` | 2 KB written → **512×** |

**The dominant cost is that a bucket read is a whole-sector read.** The
bucket-log must walk the slot log from its start to locate an id, so it pulls
the entire erase block into RAM. At 4 KB that is a 2× tax; at 64 KB it is 8×
— and it is the same reason `.bss` is 128 KB on that board. This is a
property of the **current** implementation, not of segmentation; segmentation
merely makes it visible by generating far more bucket reads per user
operation.

Three mitigations, in increasing order of value:

1. **Cache the last-read index** (one entry: owner id + the already-allocated
   `g_idx_a` table + an invalidate-on-mutation counter). Halves sequential
   read cost. O(1) RAM, ~free — the buffer already exists.
2. **Skip the cursor-finding read for prepared buckets** (§2). Removes ~40 %
   of a warm large write.
3. **Walk the slot log by streaming reads instead of whole-sector reads** —
   read the 12 B slot header + id, skip to the next, and read only the payload
   you want. This cuts read amplification on the point operations to ~1×. It is
   the highest-value change available to `blob_db` for a filesystem workload,
   and it is **independent of this proposal**.

   > **Correction, from implementing it.** This item originally also claimed it
   > "removes the reason for the two sector-sized buffers, taking `.bss` on the
   > QSPI board from 128 KB to ~1 KB". That was wrong. `count` / `iterate` and
   > compaction decide liveness by asking "does a later slot share this id?",
   > which is O(n²) in slots per bucket; served from flash that becomes O(n²)
   > *reads*, which costs far more than the sector read it replaced (on a 64 KB
   > bucket holding ~800 slots, hundreds of MB across a full scan). Those paths
   > therefore keep a resident sector image, and one sector buffer stays. The
   > read-amplification win on the hot path is real and was delivered; the RAM
   > win is smaller than stated and needs the bulk paths reworked separately.

### 3.1 What the filesystem premise changes in the base proposal

Two additions become mandatory rather than optional once `blobfs` sits
directly on L1:

- **`blob_db_write(id, offset, buf, len)` — segmented pwrite.** The base
  proposal's write path (§6.4) rewrites *all* K segments on every update. For
  a filesystem that is fatal: a 4-byte write into a 256 KB file would rewrite
  264 KB. Touching only the affected segment plus the index makes it 8 KB —
  **33× less** on 8 KB chunks, **67× less** on 2 KB chunks. `blobfs_write` is
  a pwrite by definition (`l3_interfaces.md` §4), so this is not speculative
  generality. D4 in the contract reserved `blob_db_read` but never the write
  side.
- **O(1) seek — i.e. an index, not a chain.** The `seq` container
  (`l2_containers.md` §4.1) is a linked list of chunk nodes: reaching offset X
  in a 256 KB file costs up to 131 sequential blob reads. An index record is a
  direct block table: **2 reads, independent of offset**. Same conclusion
  filesystem design reached decades ago — direct block pointers over
  FAT-style chains.

  Note this is an argument about **index vs. chain**, *not* about where the
  chunking lives. An index-based chunker layered above `blob_db` would seek in
  O(1) just as well. The reasons to put it inside L1 are different ones — see
  §3.2.

### 3.2 Why inside L1 and not a layer above it

The obvious alternative is a chunking module **above** `blob_db`, using only
its public API: split the object into N ordinary blobs, keep their ids in an
index blob, hand the caller the index blob's id. It needs no L1 change at all,
and — as §3.1 notes — it seeks in O(1) too. The current contract in fact
*prescribes* this (§5.4 D4: "large data is chained at L2"), so proposing to
move it down is proposing to reverse a recorded decision. The case for doing
so:

| | **Above L1** (chunker over the public API) | **Inside L1** (this proposal) |
|---|---|---|
| Chunk ids | user-visible; consume the caller's id space | internal; never returned by `alloc_id` |
| `count()` / `iterate()` | see every chunk as a blob — fsck, diagnostics and every other client see them too | see logical blobs only |
| Crash between chunk writes and index commit | orphan blobs that L1 **cannot** identify — they carry no owner tag and look exactly like legitimate data | one idempotent rule: a `SEGMENT` slot its owner's live index does not list is garbage |
| Reclaiming those orphans | client-side mark-and-sweep over the whole store, per client | generic sweep in `blob_db`, one implementation |
| Who implements it | every client that needs a big value: `blobfs`, `kvdb` values, `kvhash` buckets, a grown `rootreg` | once |
| Cost when unused | zero | zero (`BLOB_DB_LARGE_PAYLOADS=n`) |
| On-flash format | unchanged | additive; needs a version bump (base §11-D2) |

**The load-bearing row is orphan reclaim.** Above L1, the chunk blobs are
indistinguishable from real data: nothing on flash says "this blob is chunk 7
of blob 4711". A crash between writing the chunks and committing the index
therefore leaks blobs that no generic mechanism can ever collect — only a
client that remembers what it was doing can, which is precisely the
mark-and-sweep discipline the model container documents
(`l1_model_container.md` §4), now owed by every client that stores a large
value. Inside L1, the segment header carries `owner_id`, so a single sweep
that no client participates in cleans up after any crash. P7's "no permanent
leak (must)" is satisfied structurally rather than by convention.

The second row matters more than it looks: with chunking above L1,
`blob_db_count()` on a store holding one 256 KB file reports 132, not 1, and
`blob_db_iterate()` hands every chunk to an fsck callback that has no way to
know what it is.

**Honest counter-argument.** Keeping it above L1 preserves the narrow L1
contract (P6) and costs the core library nothing — and if exactly one client
ever needs large objects, "written once" is the same work either way. If the
decision is to keep `blob_db` minimal, the coherent version of that choice is
a **single** shared chunking module above L1 (an L1½ helper next to `rootreg`,
not a per-client protocol), which owns the index format and the sweep, and
which every large-value client uses. That gets rows 5 and 6 of the table but
still cannot get rows 2–4, because it cannot tag or hide its chunks.

---

## 4. What segmentation does *not* fix

The bucket-log has no allocator. Placement is `id mod N`, so:

- Space cannot be *chosen*. Compaction is per-bucket and cannot move a slot to
  a different bucket (the id pins it), so a partition that is 70 % full with
  unevenly filled buckets cannot be defragmented into contiguity.
- There is no locality. A file's segments are scattered across K distinct
  buckets by construction, so a sequential read is K random sector reads —
  never a contiguous burst, which is what NOR and NAND are fastest at.

Both are inherent, and both are what an **extent allocator** (contract §5.1
D1, `BLOB_DB_ALLOC_EXTENT`) exists to fix: contiguous PEB runs, a free-space
map, sequential reads at full bus speed. If the filesystem becomes the primary
workload rather than one client among several, that is the endpoint. This
proposal is the bridge to it, not a substitute — and it is a cheap bridge,
because the pread/pwrite API it introduces is exactly the API an extent
allocator would expose, so client code written against it survives the swap.

---

## 5. The constrained-device decision

The question was whether a constrained device should keep today's
implementation. Concretely, opting out saves:

| | 4 KB sectors, `MAX_PAYLOAD=256` | with Stage 2 @ K=131 (256 KB blobs) |
|---|---|---|
| `.text` + `.rodata` | 8.0 KB | 10.3 KB (**+2.3 KB**) |
| `.bss` | 8.1 KB | 10.1 KB (**+2.1 KB**) |

On a 256 KB-flash / 64 KB-RAM part that is **+0.9 % of flash and +3.3 % of
RAM**. On the same part, the two whole-sector buffers `blob_db` already holds
are **12.5 % of RAM**, and on the 64 KB-sector QSPI board they are 128 KB.

So the honest answer is:

- **Keep the Kconfig gate** — P4 requires it, it costs one `#ifdef` boundary,
  and a device that genuinely stores only 256-byte records should not carry
  the sweep and index code. But do not expect it to matter: it is ~2 KB.
- **The knob that actually matters is `MAX_SEGMENTS`**, not on/off. It is the
  single number that converts "how large a blob do I need" into RAM, at
  16 B per segment. A product needing 64 KB files pays 528 B; one needing
  505 KB pays 4 KB. Expose it, default it from `MAX_PAYLOAD_LEN / chunk`, and
  let the product decide.
- **If a device is constrained enough for 2 KB to matter, the sector buffers
  are the problem, not segmentation.** Mitigation 3 in §3 addresses that, and
  it should be ranked *above* Stage 2 for constrained targets — it is worth
  ~130 KB of RAM on the QSPI board and improves every existing operation.
- **A filesystem cannot use the current implementation at all.** A 2 KB
  maximum file is not a filesystem, so for the blobfs build this is not an
  opt-out to weigh — it is a prerequisite. The opt-out only serves builds that
  enable `blob_db` without `blobfs`.

---

## 6. Revised recommendation

Ordering changes from the base proposal, because the filesystem premise moves
one item up and adds two:

| | Change | Cost | Why this order |
|---|---|---|---|
| **1** | Stage 1 (base proposal §4): scratch-staged slots, mount-time geometry check, symbol split | ~0, negative RAM | fixes a live R7 violation; prerequisite |
| **2** | **Streaming slot walk** (§3 mitigation 3) — *new, not in the base proposal* | **+407 B `.text`** (measured), 8×→~1× read on the point ops; `.bss` unchanged (see §3 correction) | best return of anything here for the hot path, independent of large payloads |
| **3** | Stage 2 segmentation (base proposal §6) + scatter `append2` | +2.3 KB `.text`, +16 B per segment | unlocks the filesystem |
| **4** | **Segmented pwrite** `blob_db_write()` — *new* | ~0.4 KB `.text` | without it a filesystem write is O(file), not O(chunk) |
| **5** | Last-index cache (§3 mitigation 1) | ~0.2 KB `.text`, +16 B `.bss` | halves sequential read cost |
| **later** | Extent allocator (contract D1) | large, reformat | if the filesystem becomes the primary workload (§4) |

Decisions this adds to the six in the base proposal §11:

- **D7 — Adopt segmented pwrite** (`blob_db_write(id, offset, …)`) alongside
  the reserved pread. Recommendation: **yes** — it is the difference between
  8 KB and 264 KB per filesystem write.
- **D8 — Default chunk size.** Recommendation: **1024 B on 4 KB sectors,
  8 KB on 64 KB sectors** — the rows in §2 that keep per-bucket headroom.
  Trades ~12–25 % space efficiency for graceful behaviour near full.
- **D9 — Rank the streaming slot walk ahead of Stage 2.** It removes the
  read-amplification tax from every point operation and is not coupled to
  segmentation. (Its RAM claim was overstated — see the §3 correction.)
- **D10 — Inside L1, or a shared chunking helper above it (§3.2).** This is
  the layering question and it should be settled before any code. Inside L1
  is recommended, on orphan reclaim and chunk invisibility; the fallback if
  L1 must stay minimal is one shared L1½ module, **not** per-client chunking.
  Either way it reverses contract D4, which currently sends large data to an
  L2 `seq` chain.

---

## 7. Summary

The proposal is to segment large payloads **inside** `blob_db`, not to add a
chunking layer above it (§3.2) — the deciding factor is that chunks written
above L1 are indistinguishable from real blobs, so a crash mid-write leaks
data that no generic sweep can ever reclaim. That reverses contract D4 and is
the decision to settle first.

Making blobs large is cheap: **+2.3 KB of flash, +16 bytes of RAM per
supported segment**, and the code size does not grow with the payload. On the
64 KB-sector board, supporting whole-partition blobs costs 16 KB of RAM
against the 128 KB the library already holds.

Making blobs large *and* fast under a filesystem is where the real cost sits,
and it is mostly pre-existing: the bucket-log reads a whole erase block to
find one record, which is an 8× read tax and 128 KB of buffers on the QSPI
board. Segmentation does not cause that; it exposes it.

The opt-out for constrained devices is worth keeping for form's sake but is
not a meaningful lever — it saves ~2 KB either way, and a filesystem build
cannot take it. The levers that matter are `MAX_SEGMENTS` (16 B per segment,
sets the maximum file size) and the streaming slot walk (worth ~130 KB of RAM
and an 8× read reduction, and worth doing before segmentation rather than
after).
