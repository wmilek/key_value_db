# Design change proposal — large payloads in `blob_db`

Status: **proposal / for review** · 2026-08-09
· Target contract: `doc/layers/l1_blob_db.md` (§3 R6/R7, §4, §5.4 D4)
· Target implementation design: `doc/impl/l1_bucketlog.md` (§3, §5, §7, §13)
· Governed by `doc/principles.md`
· **Costed, and its recommendation revised, in
  `doc/proposals/2026-08-09-large-payloads-cost.md`** — read that addendum
  alongside §5 and §11 here: it measures the feature (+2.3 KB `.text`,
  16 B `.bss` per segment), and under a filesystem workload it adds a
  segmented pwrite (D7), fixes the default chunk size (D8), and ranks a
  streaming slot walk *ahead* of Stage 2 (D9).

**The ask.** `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is 256 B by default and capped
at 4096 B by its Kconfig range. Can it be raised to **hundreds of kilobytes**?

**Short answer.** Not by changing the Kconfig range. The 4096 in the range is
the *loosest* of five independent limits; the binding ones are structural
(§2). Raising the number alone buys at most ~2 KB of *sustainably rewritable*
payload on the `native_sim` geometry and ~32 KB on the nRF5340-DK QSPI NOR,
and it pays for every byte twice in `.bss`. Hundreds of kilobytes requires a
payload that spans more than one erase block — a **segmented payload**
representation inside L1, plus the partial-access API that contract §5.4 (D4)
already reserved for exactly this moment. §5 recommends a two-stage plan; §6
specifies the design; §11 lists the decisions this proposal needs from review.

---

## 1. Where the limit is today

Effective cap = `min(` C1, C2, C3 `)` from the table below, and C4–C7 decide
what that cap *costs*.

| # | Constraint | Where | Ceiling it imposes |
|---|---|---|---|
| **C1** | Kconfig `range 1 4096` | `lib/blob_db/Kconfig:49` | 4096 B — pure policy, no structure behind it |
| **C2** | `val_len` is `uint16_t` | `lib/blob_db/blob_db_internal.h:84` | 65 535 B |
| **C3** | A slot must fit inside one PEB, after the 16 B bucket header | `blob_db.c:490`, `:507`, `:950` | `peb_size − 16 − 14` |
| **C4** | `append_slot()` stages the whole slot in a **stack** buffer | `blob_db.c:593` | stack frame = `MAX_PAYLOAD + 46` — breaks contract R7 ("≤ one erase sector, 4 KB target") |
| **C5** | Two whole-sector scratch buffers in `.bss` | `blob_db.c:55-56`, sized by `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` | `.bss = 2 × peb_size` — raising the cap *via bigger erase blocks* costs 2 B of RAM per byte of payload |
| **C6** | `get()` copies the whole payload into the caller's buffer | `blob_db.c:856` | the **caller** needs `MAX_PAYLOAD` bytes of RAM, for every read |
| **C7** | `kvhash` keeps two static `MAX_PAYLOAD` buffers | `lib/containers/kvhash/kvhash.c:56-57` | `.bss += 2 × MAX_PAYLOAD` in that module alone |
| **C8** | Tests and the model container declare `MAX_PAYLOAD` arrays on the stack | `tests/lib/blob_db/src/main.c:184-204`, `tests/lib/blob_db_contract/src/model_container.c:69,97,105,134` | stack, per test |
| **C9** | Per-slot integrity is CRC16-CCITT | `blob_db.c:449` | 16 bits of detection over a slot that would grow 250× |
| **C10** | `rootreg` asserts its image fits a payload | `lib/rootreg/rootreg.c:47` | a *floor*, not a ceiling — unaffected |

Concretely, per geometry:

| Geometry | `peb_size` | C3 ceiling | `.bss` for C5 | Buckets in 8 MB |
|---|---|---|---|---|
| `native_sim`, nRF internal NVMC | 4 KB | **4066 B** | 8 KB | 2045 |
| MX25R64 QSPI NOR (nRF5340-DK) | 64 KB | **65 506 B** (C2 clamps at 65 535) | 128 KB | 125 |

### 1.1 The sustainable-rewrite limit is half of C3

C3 is the limit for *binding* a payload once. Rebinding is stricter. A bucket
is an append-only log: a rebind appends a second slot and only a later
compaction reclaims the first. So a rebind needs `2 × slot_size` to fit
between the bucket header and the sector end:

```
sustainable inline max = ⌊(peb_size − 16) / 2⌋ − 14
                       = 2026 B   (4 KB sectors)
                       = 32 746 B (64 KB sectors)
```

Above that, the first `update` succeeds and every later one hits
`compact_bucket()` → the compacted image is the same size → `-ENOSPC`
(`blob_db.c:963`). A blob you can write once and never change is not what the
contract promises (§2 "Id stability": `update` keeps the id, only the payload
changes).

**So the honest headline number today is ~2 KB, not 4 KB** — and the Kconfig
range of 4096 already over-promises on the default geometry.

---

## 2. Why raising the number cannot reach hundreds of kilobytes

Three structural walls, in order of how hard they are to move:

1. **One slot lives in one erase block (C3).** This is the core of the
   bucket-log design: `id → bucket = id mod N`, one bucket = one PEB, one
   `get` = one sector read (`l1_bucketlog.md` §1). A 256 KB payload cannot
   occupy one PEB unless the PEB is ≥ 256 KB — which no part in this project
   has, and which would put `.bss` at 512 KB via C5.

2. **`val_len` is 16 bits (C2).** 65 535 B is a hard ceiling on any
   single-slot representation regardless of geometry. Widening it is an
   on-flash format break for *every* slot, including the millions of small
   ones the design is optimized for.

3. **Both API sides are whole-blob (C6).** Even if the bytes fit on flash,
   `blob_db_get(id, out, out_sz, …)` demands a `total_len` buffer from the
   caller and `blob_db_update(id, payload, len)` demands one from the writer.
   At 256 KB that is half the nRF5340's application RAM per side, for a
   library whose first principle after Zephyr is "minimum RAM" (P3) and whose
   R1 is "steady-state RAM is O(1)". **Raising the cap without partial access
   moves the RAM problem from the library to every client, where it is
   worse.**

Trading erase-block size for payload size also degrades the store as a whole:
at 64 KB sectors an 8 MB partition has **125 buckets**, so contract R6's
"100 000 blobs in an 8 MB partition" means 800 blobs per bucket in a
64 KB log — and every `get` reads and walks 64 KB to find one of them.

---

## 3. Options considered

| | **A — raise the cap** | **B — segmented payloads in L1** *(recommended)* | **C — new allocator (extent)** | **D — chain at L2 (status quo D4)** |
|---|---|---|---|---|
| Max payload | `⌊(peb−16)/2⌋−14`: 2 KB / 32 KB | partition-bounded; ~2 MB single-level index on 4 KB sectors | partition-bounded | partition-bounded |
| On-flash format change | none | additive (new slot flags + index record) | **full break**, reformat | none |
| L1 RAM | +2 B `.bss` per payload byte (C5) | **unchanged** (≤ 1 sector) | new: free-space map | unchanged |
| Client RAM per access | = payload | O(1) with pread | O(1) with pread | O(chunk) |
| Small-blob cost | unchanged | **unchanged** (P4: feature off = zero cost) | worse (i-node indirection on every get) | unchanged |
| Effort | ~1 day | ~1–2 weeks | ~4+ weeks | 0 (already the plan) |
| Reaches "hundreds of KB" | ✗ | ✓ | ✓ | ✓, but not as a blob_db payload |

**Why not D.** D4 currently says large data is chained at L2 by the `seq`
container. That remains right for *streams* (logs, files), but it does not
answer the request: a kvdb value, a rootreg image, or a kvhash bucket is a
single logical payload, and pushing it to `seq` means every client above L1
grows a chunking protocol and its own crash discipline. The proposal is to
move that one mechanism **down** into L1, where it is written once and
inherits the existing atomicity primitives.

**Why not C now.** An extent allocator is the right shape when the workload
becomes *few large blobs*; contract §5.1 (D1) already reserves
`BLOB_DB_ALLOC_EXTENT` for that. It needs PEB-granular free-space accounting,
which the bucket-log deliberately does not have (`l1_bucketlog.md` Appendix
B), and it costs a reformat. Option B reaches the same payload sizes on the
allocator we already ship and already trust; C stays the escape hatch if
large blobs become the dominant workload rather than an occasional one.

---

## 4. What Option A should still deliver

Even under Option B, C4 is a defect worth fixing on its own: `append_slot()`
puts `MAX_PAYLOAD + 46` bytes on the stack, so today's 4096 setting already
means a ~4.1 KB stack frame — against R7's 4 KB target. And C1's range is
wrong in both directions (it permits 4096 where 2026 is sustainable, and
forbids 32 746 where the hardware allows it).

Stage 1 is therefore small and independently useful:

- Stage the slot in the existing `.bss` scratch buffer instead of the stack —
  `g_bbuf_new` is sector-sized, already allocated, and provably idle at every
  `append_slot()` call site (compaction finishes before the append). Net RAM
  change: **−`MAX_PAYLOAD` of stack, +0 `.bss`.**
- Replace the Kconfig `range 1 4096` with `range 1 65535` and enforce the real
  bound **at mount**, where `peb_size` is known:
  `MAX_INLINE_LEN ≤ ⌊(peb_size − 16)/2⌋ − 14`, else `-ENOTSUP` with a log line
  naming both numbers. Geometry is a runtime property (P2); a compile-time
  range cannot express it.
- Split the symbol in two (see §7) so clients that size buffers off the cap
  (C7, C8) keep sizing off the *inline* bound.

That lands ~2 KB (native_sim) / ~32 KB (QSPI NOR) with no format change, and
it is a prerequisite for Stage 2 rather than throwaway work.

### 4.1 Make every future format change detectable — and refuse, don't reformat

A separate requirement, raised in review and worth stating as a standing
property rather than a fix for this proposal: **software must always be able
to detect that a store was written in a format it does not understand, and
decide what to do about it.** Today it cannot, and the gap is already a
contract violation.

**The live defect.** Contract §4 lists `-ENOTSUP` ("foreign on-flash format")
and §5.1 D1 states that "each allocator uses a distinct on-flash magic so a
mismatched mount fails cleanly with `-ENOTSUP`". The implementation does the
opposite: `read_master()` reports *invalid* for both a wrong magic and a bad
CRC, and `blob_db_mount()` treats "neither master valid" as "virgin
partition" and formats (`blob_db.c:304`). A store written by another
allocator — or by a future version of this one — is therefore **erased on
first mount by older firmware**, silently. This is true of the code on `main`
today, with or without large payloads.

**Why a plain version field is not enough.** Adding `version` somewhere in
`blob_db_master_hdr` does not solve it, because `hdr_crc32` lives at
`sizeof(hdr) - 4` and covers everything before it (`hdr_crc32()`,
`blob_db.c:79`). The moment the header grows, the CRC's span and position
both move, so old code cannot validate the header at all — it fails the CRC
check *before* it ever reaches the version field, and lands right back in the
reformat path. The discriminator has to be readable and verifiable **without
knowing the header's size**.

**Mechanism — a frozen self-describing prefix.** Split the master header into
a small prefix that is guaranteed never to change again, plus a
version-specific body:

```c
/* Frozen for all time. Any blob_db, of any vintage, can parse exactly this
 * much and reach a decision. */
struct __packed blob_db_compat_hdr {   /* 12 B */
	uint8_t  magic[4];        /* allocator identity (contract D1)        */
	uint8_t  format_major;    /* incompatible — refuse if > known        */
	uint8_t  format_minor;    /* additive — unknown extras are ignorable */
	uint16_t hdr_len;         /* total master header length              */
	uint16_t reserved;
	uint16_t prefix_crc16;    /* CCITT over the leading 10 B             */
};
/* then: generation, state, compacting_bid, next_id_hint, [seg_owner], hdr_crc32 */
```

Two CRCs, deliberately: `prefix_crc16` is what lets *any* version trust
`format_major` without understanding the body, and the existing `hdr_crc32`
keeps covering the body for code that does. Mount becomes:

```
read 12 B prefix from master A and B
  prefix CRC bad on both, and both sectors read all-0xff  -> virgin: format
  prefix CRC bad on both, sectors NOT erased              -> -ENOTSUP: refuse
  wrong magic                                             -> -ENOTSUP: refuse
  format_major > BLOB_DB_FORMAT_MAJOR                     -> -ENOTSUP: refuse
  otherwise: read hdr_len bytes, verify hdr_crc32, proceed
```

Three consequences worth being explicit about:

- **Erased is not the same as unrecognized.** Distinguishing them is the whole
  fix: virgin flash reads all-`0xff` and should be formatted; a sector holding
  bytes we cannot parse must never be. That check is what turns silent
  destruction into a clean refusal.
- **Pick the winner before checking the version.** With A and B at different
  majors (an interrupted upgrade), choose the highest generation among prefixes
  that pass, then apply the major check to *that* one. Falling back to the older
  master would silently mount a stale view of an upgraded store.
- **`format_major` is checked, not the header size.** Growing the body for an
  additive change bumps `format_minor` and `hdr_len`; old code reads the fields
  it knows and ignores the tail. Only genuinely incompatible changes bump
  `format_major`, and those are the ones old code refuses.

**Do it in Stage 1, now.** This reshapes the master, which is a breaking change
— and it is the *last* undetectable one, because after it every future change
announces itself. There is no deployed hardware, so the cost today is zero and
falls to "flash-day migration" the moment there is. It also closes
`l1_bucketlog.md` §13.4 and finally delivers the `-ENOTSUP` the contract has
promised since v1.

One judgement call to leave to the implementation: a store whose prefix parses
and whose *body* CRC fails on both masters is corruption, not foreignness — a
power loss during the very first format can produce it. Refusing there is
correct but turns a bad first boot into a device needing explicit
`blob_db_format()`. Suggest gating that one case on a Kconfig
(`BLOB_DB_AUTOFORMAT_ON_CORRUPT`, default `y` for development, `n` for
production) rather than hard-coding either answer.

---

## 5. Recommendation

**Stage 1 — inline cap, done correctly** (no on-flash change, no API change).
§4. Ships the honest maximum for a single-slot payload.

**Stage 2 — segmented payloads** (additive on-flash change, additive API).
§6. Ships hundreds of kilobytes, with L1 RAM unchanged and client RAM O(1).

Both stages sit behind Kconfig so a build that does not want large payloads
pays nothing (P4).

---

## 6. Stage 2 design — segmented payloads

### 6.1 Shape

A payload longer than `BLOB_DB_MAX_INLINE_LEN` is stored as **K segment
blobs plus one index slot**:

- Each **segment** is an ordinary slot in an ordinary bucket, flagged
  `BLOB_DB_SLOT_F_SEGMENT`, holding one chunk of the payload. Its id comes
  from the same allocator counter but is never returned to a caller.
- The **index** is an ordinary slot **at the user's id**, flagged
  `BLOB_DB_SLOT_F_INDEXED`, whose payload is the segment table.

Everything the bucket-log already does — latest-wins, CRC, tombstones,
per-bucket compaction, the write cursor walk — applies to both kinds of slot
unchanged. The index slot *is* the blob as far as `id → bucket = id mod N`,
`delete`, and compaction are concerned. Nothing on the small-blob path
changes: an inline payload keeps today's exact bytes on flash.

```
user id 4711  ──►  bucket 4711 mod N
                   ┌──────────────────────────────────────┐
                   │ slot: flags=SEALED|INDEXED           │
                   │ payload = index record               │
                   │   total_len, crc32, seg_len,         │
                   │   seg_id[0..K-1]                     │
                   └──────────┬───────────────────────────┘
                              │ seg_id[j] mod N
                              ▼
                   ┌──────────────────────────────────────┐
                   │ slot: flags=SEALED|SEGMENT           │
                   │ payload = { owner_id, seq } + chunk  │
                   └──────────────────────────────────────┘
```

### 6.2 On-flash records (additive)

New slot flag bits, in the existing `flags` byte (`blob_db_internal.h:46`):

```c
#define BLOB_DB_SLOT_F_INDEXED    (1u << 2)  /* payload is an index record */
#define BLOB_DB_SLOT_F_SEGMENT    (1u << 3)  /* payload is a chunk of another blob */
```

Index record (the indexed slot's payload):

```c
struct __packed blob_db_index_hdr {   /* 16 B */
    uint8_t  magic[2];       /* 'B','X'                                    */
    uint8_t  version;        /* 1                                          */
    uint8_t  flags;          /* reserved, 0                                */
    uint32_t total_len;      /* logical payload length                     */
    uint32_t payload_crc32;  /* CRC32-IEEE over the reassembled payload    */
    uint16_t seg_count;      /* K                                          */
    uint16_t seg_len;        /* bytes per segment; the last one may be short */
};
/* followed by uint64_t seg_id[seg_count] */
```

Segment prefix (first bytes of a segment slot's payload):

```c
struct __packed blob_db_seg_hdr {     /* 12 B */
    uint64_t owner_id;       /* the user-visible id this chunk belongs to  */
    uint16_t seq;            /* chunk index, 0..K-1                        */
    uint16_t reserved;
};
```

`owner_id` is what makes orphan reclaim possible without any RAM index
(§6.5). Putting it in the segment's *payload* rather than in the slot header
keeps `struct blob_db_slot_hdr` at 4 B, so the format of the millions of
small slots is untouched.

**Capacity.** A segment slot carries `12 + seg_len` bytes, so the chunk size
is `seg_len = inline_max − 12`. The index is `16 + 8K` bytes and must itself
fit one inline payload, giving `K ≤ (inline_max − 16) / 8`:

| Geometry | inline max | `seg_len` | max K | **max blob** |
|---|---|---|---|---|
| 4 KB sectors | 2026 B (sustainable) | 2014 | 251 | **~505 KB** |
| 4 KB sectors | 4066 B (bind-once) | 4054 | 506 | ~2.0 MB |
| 64 KB sectors | 32 746 B | 32 734 | 4091 | > partition |

505 KB on the default `native_sim` geometry satisfies "hundreds of kilobytes
at least" with a single-level index. A two-level index would lift it further
and is deliberately **not** proposed — partition size becomes the binding
limit first.

**Overhead.** 14 B slot header + 12 B segment header + write-align padding per
2014 B chunk ≈ **1.3 %** on 4 KB sectors, 0.08 % on 64 KB sectors.

### 6.3 Read path

`get()` keeps its meaning: on an indexed slot it gathers all K segments into
the caller's buffer, or returns `-ENOMEM` if `out_sz < total_len` (unchanged
semantics, and the honest answer when the caller cannot hold the blob).

The usable path for large blobs is the partial access that contract §5.4 (D4)
already specified:

```c
int blob_db_size(uint64_t id, size_t *out_size);
int blob_db_read(uint64_t id, size_t offset, void *out, size_t len,
                 size_t *out_read);
```

`blob_db_read` on an indexed blob: 1 sector read for the index, then
`seg = offset / seg_len`, 1 sector read for that segment, copy. **Two sector
reads per call, O(1) caller RAM** — R2 ("`get` cost is independent of database
size") holds. On an inline blob it is one read, and `blob_db_size` is one read
in both cases.

### 6.4 Write path and the commit point

`update(id, payload, len)` with `len > inline_max`:

```
0. read the id's live slot; if INDEXED, remember its seg_id[] (≤ 1 inline payload of RAM)
1. master: set seg_owner = id                    ── enters the sweep window
2. for j in 0..K-1: alloc internal id, append segment slot {owner=id, seq=j, chunk}
3. append the INDEXED slot at id                 ── ATOMIC COMMIT (latest-wins, §4)
4. tombstone the segments remembered in step 0
5. master: clear seg_owner                       ── leaves the sweep window
```

Step 3 is the single commit write, exactly as D4 requires ("a multi-chunk
write must commit by writing the id's index record **last**"). Everything
before it is invisible; everything after it is garbage collection. Contract §2
"Atomicity" and "No partial reads" therefore hold with no new mechanism — the
same slot-append primitive that makes small writes atomic makes large ones
atomic.

`delete(id)` on an indexed blob: set `seg_owner`, tombstone the index
(**commit**), tombstone the segments, clear `seg_owner`.

### 6.5 Crash recovery — one rule

A crash anywhere in §6.4 leaves *unreferenced segments*, never wrong data:

| Crash point | State on flash | Why it is safe |
|---|---|---|
| in step 2 | new segments written, old index still live | new segments are unreferenced; the old payload reads correctly |
| in step 3 | index slot torn | CRC fails → end-of-log → old index still live (`blob_db.c:518`) |
| in step 4 | new index live, old segments still present | old segments are unreferenced; the new payload reads correctly |

One idempotent rule reclaims all three:

> **A slot flagged `SEGMENT` is garbage unless its `owner_id`'s currently live
> slot is an index record that lists this segment's id.**

Mount runs the sweep only when the master carries a non-zero `seg_owner`
(so the normal path pays nothing): stream every bucket through the existing
`g_bbuf`, hold the owner's index in `g_bbuf_new`, tombstone any `SEGMENT`
slot for that owner that the index does not list. **N sector reads, O(1)
RAM, idempotent** — satisfying R4 and P7's "no permanent leak (must)" with
bounded, restartable recovery, in the same shape as the existing compaction
recovery (`recover_compaction()`, `blob_db.c:780`).

`seg_owner` must be a *field*, not a `state` value, because a segment append
in step 2 can itself trigger `compact_bucket()` and thus
`BLOB_DB_STATE_COMPACTING`; the two recovery obligations must be able to be
outstanding at once. That grows `blob_db_master_hdr` from 24 B to 32 B, which
is why §11-D2 asks for the format-version field that `l1_bucketlog.md` §13.4
already has open.

### 6.6 Visibility of internal ids

Segment ids are consumed from the `alloc_id` counter but never returned, so
contract §2 ("never returned before, strictly greater than every previously
returned id") is unaffected. Three call sites must learn to skip them:

- `for_each_live_slot()` (`blob_db.c:1075`) — skip `SEGMENT` slots, so
  `count()` and `iterate()` continue to report *logical* blobs only.
- `get()` / `exists()` on a segment id — defensively `-ENOENT` / `false`
  (calling them is already UB: the id was never returned by `alloc_id`).
- `update()` / `delete()` on a segment id — `-EINVAL`.

Compaction needs no change: a segment slot is copied or dropped by the same
latest-wins/tombstone rules as any other slot, and ids never move (contract §2
"Reorganization transparency").

### 6.7 Streaming writes

`update(id, payload, len)` still requires the caller to hold `len` bytes. For
a 256 KB blob assembled incrementally that is the same RAM problem as C6, on
the write side. Proposed companion API (**new decision — see §11-D1**):

```c
struct blob_db_writer;   /* opaque handle; library-owned state, O(1) caller RAM */

int blob_db_write_begin (struct blob_db_writer **w, uint64_t id, size_t total_len);
int blob_db_write_chunk (struct blob_db_writer  *w, const void *buf, size_t len);
int blob_db_write_commit(struct blob_db_writer  *w);   /* performs step 3 */
int blob_db_write_abort (struct blob_db_writer  *w);
```

One writer may be open at a time (`-EBUSY` otherwise), consistent with the
single-threaded v1 contract; the segment buffer and the growing `seg_id[]`
table live in the library's existing sector scratch, so no new `.bss`. Commit
is the same step 3; abort leaves the sweep window to clean up.

### 6.8 Cost summary — 256 KB blob, 4 KB sectors, `seg_len` 2014 (K = 131)

| Op | Flash reads | Flash writes | RAM |
|---|---|---|---|
| `update` (first bind) | 131 bucket reads (walk) + 1 | 1 master + 131 slots + 1 index + 1 master | ≤ 1 sector |
| `update` (rebind) | above + 1 index read | above + 131 tombstones | ≤ 1 sector |
| `read(offset, len)` within one segment | **2** | 0 | ≤ 1 sector |
| `size` | 1 | 0 | O(1) |
| `get` (whole) | 1 + 131 | 0 | caller: 256 KB |
| `delete` | 1 + 131 | 1 master + 1 index + 131 tombstones + 1 master | ≤ 1 sector |
| `mount` after a clean shutdown | unchanged | unchanged | unchanged |
| `mount` inside a sweep window | + N (2045) | ≤ K tombstones | ≤ 2 sectors |

Cold-store writes additionally pay one sector erase per untouched bucket;
`blob_db_prepare()` (already implemented) amortizes that off the hot path,
and its value grows sharply under this proposal — a large write now touches
K buckets instead of one.

---

## 7. Kconfig

`BLOB_DB_MAX_PAYLOAD_LEN` today means "the largest single slot". Segmentation
introduces a second, larger quantity — "the largest logical object" — and the
two must not share a name.

**Do not redefine the existing symbol.** An earlier draft of this section
proposed promoting `BLOB_DB_MAX_PAYLOAD_LEN` to the logical cap and adding
`BLOB_DB_MAX_INLINE_LEN` for the old meaning. That is unsafe: `kvhash` derives
*three* things from it (`kvhash.c:45-55`) —

```c
#define MAX_BUCKETS  ((MAX_PAYLOAD - DIR_HDR_LEN) / 8u)   /* directory capacity */
static uint8_t dir_buf[MAX_PAYLOAD];                      /* .bss */
static uint8_t bkt_buf[MAX_PAYLOAD];                      /* .bss */
```

— so a build that raised the symbol to 262 144 without touching `kvhash`
would compile cleanly and then (a) allocate 512 KB of `.bss`, (b) let a new
map be created with 32 767 buckets instead of 31, and (c) let one hash bucket
pack 256 KB, silently turning every kvhash bucket into a segmented blob. A
rename that fails *loudly* would be fine; this one fails silently, and the
same trap waits for any out-of-tree client.

So the existing symbol keeps its exact current meaning, default and value, and
the new quantity gets a new name:

```
config BLOB_DB_MAX_PAYLOAD_LEN         # UNCHANGED meaning: largest single slot
	int "Max single-slot payload length in bytes"
	default 256
	range 1 65535                  # C2; the real bound is checked at mount (§4)

config BLOB_DB_LARGE_PAYLOADS
	bool "Objects larger than one slot (segmented)"
	default n                      # P4: off = today's code, byte for byte

config BLOB_DB_MAX_OBJECT_LEN          # NEW: the logical cap
	int "Max logical object length in bytes"
	depends on BLOB_DB_LARGE_PAYLOADS
	default 262144
	range BLOB_DB_MAX_PAYLOAD_LEN 16777216

config BLOB_DB_SEGMENT_LEN
	int "Segment size (0 = auto, per geometry — see cost addendum D8)"
	depends on BLOB_DB_LARGE_PAYLOADS
	default 0

config BLOB_DB_MAX_SEGMENTS
	int "Max segments per object (sets .bss: 16 B each)"
	depends on BLOB_DB_LARGE_PAYLOADS
	default 0                      # 0 = derive from MAX_OBJECT_LEN / segment size
```

Consequences, and they are all good ones:

- **No in-tree client changes at all.** `kvhash`, `rootreg`, `app_perf`, and
  both test suites keep compiling untouched, with identical buffer sizes and
  identical on-flash behaviour. The migration table this section used to carry
  is empty.
- Existing clients keep their slot-sized buffers by default, which is what
  they want — none of them should grow to hundreds of kilobytes.
- A client that *wants* large objects opts in explicitly by referencing
  `MAX_OBJECT_LEN` and the pread/pwrite API. Opting in is visible in the
  source rather than implied by a Kconfig value.
- The only doc change is `include/app/lib/blob_db.h:166`, which should now say
  `len` is bounded by `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`, or by
  `CONFIG_BLOB_DB_MAX_OBJECT_LEN` when `BLOB_DB_LARGE_PAYLOADS` is enabled.

### 7.1 What a small-object client sees

Nothing, by design. Spelled out because it is the first question any existing
user asks:

| | `LARGE_PAYLOADS=n` | `LARGE_PAYLOADS=y`, payload ≤ slot |
|---|---|---|
| `alloc_id` / `get` / `update` / `delete` / `exists` / `count` / `iterate` / `format` / `erase_all` / `prepare` | unchanged | unchanged |
| Bytes written to flash for a small blob | unchanged | **byte-identical** — no `INDEXED`/`SEGMENT` bits set |
| `.text` / `.bss` | unchanged | +2.3 KB / +16 B per configured segment |
| Extra work on the write path | none | one `len > inline_max` comparison |
| Kconfig symbols they use | unchanged | unchanged |

Segmentation engages only when a single `update` exceeds the slot bound. A
store that never stores a large object is indistinguishable, on flash and at
the API, from one built without the feature.

## 8. Contract impact (`doc/layers/l1_blob_db.md`)

| Clause | Impact |
|---|---|
| §2 stability contract | **unchanged.** Index-last commit preserves atomicity and "no partial reads"; ids are still opaque, stable, never reused. |
| §3 R1 (O(1) steady-state RAM) | **held.** No per-blob RAM; the writer's state is one sector of already-allocated scratch. |
| §3 R2 (`get` independent of DB size) | **held** for `read`/`size` (2 reads); whole-blob `get` becomes O(K) in the blob's own size, which is inherent. |
| §3 R6 ("payload cap is a Kconfig option; no structural limit other than partition size") | **finally delivered.** Today's implementation does not satisfy R6 as written — this is the gap the proposal closes. |
| §3 R7 (transient buffers ≤ one sector) | **held**, and Stage 1 fixes an existing violation (C4). |
| §4 public API | **grows** by `blob_db_size` / `blob_db_read` (already reserved by D4) and, pending §11-D1, the streaming writer. |
| §5.4 D4 | **amended.** "v1 keeps L1 payloads single-chunk; large data is chained at L2" becomes "L1 chunks transparently when `BLOB_DB_LARGE_PAYLOADS` is on; L2 `seq` remains the answer for *streams*, not for large single values." |
| §5.1 D1 (exchangeable allocators) | **respected.** Segmentation is a bucket-log implementation detail; a future extent allocator satisfies the same enlarged API differently. |

`doc/impl/l1_bucketlog.md` takes the mechanical changes: §3 (new flags, index
and segment records, 32 B master), §5 (write/read/sweep algorithms), §6.1
(sweep row in the crash table), §7 (Kconfig), §13 (closes 13.3 sector-size
portability, closes 13.4 format version, adds the sweep as a tracked item).

---

## 9. Test plan

Extends `tests/lib/blob_db/` (unit) and `tests/lib/blob_db_contract/`
(acceptance, crash injection):

1. Boundary matrix around `inline_max`: `inline_max−1`, `inline_max`,
   `inline_max+1` (first segmented), `2×seg_len`, `2×seg_len+1`, declared max.
2. Round-trip of a 256 KB payload with a known pattern; verify
   `payload_crc32`; verify via `read()` at every segment boundary and at
   unaligned offsets straddling two segments.
3. Rebind large→large, large→small, small→large; assert `count()` is
   unchanged and no `SEGMENT` slot survives from the previous generation.
4. `delete` of a large blob, then `count()`/`iterate()` see nothing, and a
   full-bucket scan finds no live `SEGMENT` slot for that owner.
5. Crash injection at each of the five steps of §6.4 (the contract suite
   already has the harness): remount, then assert (a) the payload is either
   fully old or fully new, and (b) after the sweep, zero unreferenced
   segments — the P7 "no permanent leak" check.
6. Sweep idempotence: force two consecutive crashes inside the window.
7. `-ENOSPC` behaviour: a payload larger than the free partition fails
   cleanly and leaves no residue after the sweep.
8. RAM regression guards: `BUILD_ASSERT` on the writer/scratch sizes, and a
   stack high-water check on `native_sim` proving R7 still holds at the
   declared maximum.
9. `BLOB_DB_LARGE_PAYLOADS=n` build produces byte-identical on-flash images
   for the existing suite (the P4 "costs nothing when unused" check).

---

## 10. Compatibility

**Existing stores and existing small-object clients are unaffected** — see
§7.1 for the API/build side. On flash:

- **Stage 1** reshapes the master around the frozen compatibility prefix
  (§4.1) and sets `format_major = 1`. This is a **breaking change**: stores
  written by today's code are not readable afterwards. With no deployed
  hardware that costs nothing, and it is deliberately taken now because it is
  the last change that older software cannot detect.
- **Stage 2** is additive for slots — a store holding only inline payloads is
  byte-identical, since the new flag bits are never set. The master body grows
  to carry `seg_owner` (§6.5), which bumps `format_minor` and `hdr_len`, not
  `format_major`: Stage-1 software mounting a Stage-2 store reads the fields it
  knows and ignores the tail.

After Stage 1 the compatibility story is a property of the format rather than
of any particular change: an unknown `format_major` is refused with
`-ENOTSUP`, an unknown `format_minor` is tolerated, and an unparseable sector
that is not erased is never reformatted. Downgrade across a `format_major`
bump is not supported, but it is now *detected and refused* rather than
silently destructive.

---

## 11. Decisions needed from review

- **D1 — Streaming write API (§6.7).** Adopt `write_begin/chunk/commit/abort`,
  or keep writes whole-blob and accept that a 256 KB write needs a 256 KB
  caller buffer? Recommendation: **adopt** — without it the write side
  reproduces exactly the RAM problem the proposal exists to solve.
- **D0 — Frozen compatibility prefix (§4.1).** Reshape the master header now
  so that every future format change is detectable by older software, and make
  mount refuse an unparseable store with `-ENOTSUP` instead of reformatting it.
  This is a breaking change and the last undetectable one. Recommendation:
  **yes, in Stage 1** — it is free today, it closes a standing contract
  violation, and it subsumes D2.
- **D2 — Master format version (§6.5, §10)** — *subsumed by D0 if that is
  adopted; kept here as the narrower fallback.* Introduce the `version` field in
  **Stage 1**, reusing the existing `reserved` byte so the master stays 24 B
  and nothing on flash changes size (closing `l1_bucketlog.md` §13.4). The
  32 B master carrying `seg_owner` then arrives with Stage 2. Doing the
  version byte early is what lets fielded firmware refuse a future segmented
  store instead of reformatting it (§10). Recommendation: **yes**, and treat
  the Stage-1 half as a prerequisite for shipping Stage 2 to any device that
  is already deployed.
- **D3 — Segment id namespace.** Segments consume ids from the same counter
  (proposed) versus a reserved high range (e.g. bit 63 set). Recommendation:
  **same counter** — 2⁶⁴ is not a scarce resource, and a reserved range adds a
  second allocation path with its own durability story.
- **D4 — Whole-blob `get` on a large blob.** Keep it (returning `-ENOMEM` when
  the caller's buffer is short), or make it `-EFBIG` above a threshold to push
  callers onto `read()`? Recommendation: **keep** — `-ENOMEM` already carries
  the right meaning and small "large" blobs are legitimately gettable.
- **D5 — Payload CRC32 in the index (§6.2).** Costs 4 B and one pass over the
  payload; buys end-to-end detection across a 131-segment gather, where the
  per-slot CRC16 (C9) only protects each chunk in isolation. Recommendation:
  **include**.
- **D6 — Default `BLOB_DB_MAX_INLINE_LEN`.** Keep 256 (no behavioural change
  for existing builds) or raise to the sustainable geometric maximum?
  Recommendation: **keep 256**; the mount-time check (§4) makes raising it a
  per-product decision with an honest failure mode.

---

## 12. Summary

`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` cannot reach hundreds of kilobytes by
widening its range: the binding limits are a 16-bit length field, a slot that
must fit one erase block, and a whole-blob API on both sides. Raising the
number alone tops out near 2 KB of sustainably rewritable payload on the
default geometry — and buying more through larger erase blocks costs two bytes
of `.bss` per payload byte while collapsing the bucket count.

Segmenting payloads inside L1 — K ordinary segment slots plus an index slot
written last — reaches ~505 KB on the default geometry and past the partition
on QSPI NOR, keeps L1's transient RAM at one sector, keeps client RAM O(1)
through the pread API that D4 already reserved, and leaves the small-blob path
and the stability contract untouched. It is an additive on-flash change on the
allocator already in production, with one new idempotent recovery rule
("an unreferenced segment is garbage") covering every crash point.

Recommended: Stage 1 now (a genuine R7 fix plus an honest cap), Stage 2 behind
`CONFIG_BLOB_DB_LARGE_PAYLOADS` once §11's six decisions are settled.
