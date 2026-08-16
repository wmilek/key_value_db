# What `blob_db` would gain from being written for UBI

**Status: analysis, nothing implemented.** Every figure is either a measured
counter from `doc/proposals/2026-08-16-persondb-case-performance.md` or
arithmetic on one, and each is labelled. No hardware.

The question this answers: given a free hand — expand the porting interface,
duplicate the implementation, change the on-flash format — **what does UBI
actually buy `blob_db`, and what does it not?**

The short version: **one large win, one hard blocker nobody would find until
mid-implementation, and one thing that looks like a UBI win but is not.**

---

## 1. What UBI uniquely buys

| UBI primitive | `blob_db` mechanism it replaces |
|---|---|
| **atomic whole-LEB replace** (`ubi_leb_write`) | the entire `compact_commit()` protocol: master `COMPACTING`, scratch erase/write/seal, bucket erase/write, scratch erase, master `CLEAN` — plus `recover_compaction()` and the wedge state |
| wear-levelling | nothing — `blob_db` has none. On raw flash the masters and scratch are erased on *every* compaction anywhere in the store, ~250× a data bucket's rate |
| bad-block handling | nothing — `blob_db` has none |
| free-pool + `unmap` | a tombstone plus a wait for compaction |

**And what it does not buy: anything on the read path.** A LEB is an erase
block. UBI's granularity is exactly raw flash's granularity, so the cost that
dominates every read case — one flash transaction per blob sharing a 64 KB
sector — is untouched by it. That is `blob_db`'s own design and only `blob_db`
can fix it (§5).

Worse, today UBI *doubles* physical read transactions: `ubi_plain_leb_read()`
reads and CRCs the VID header on every call, uncached. Measured exactly 2.000×
(`persondb-case-performance.md` §12.2a).

---

## 2. The win: compaction goes from five erases to one

Measured on `native_sim` at 10 000 persons: **4 924 erases per fill**, which is
985 compactions × 5. `compact_commit()` spends four of those five on making the
fifth survivable — staging into scratch because erasing the bucket destroys the
only copy, and two master writes to record the intent.

`leb_prepare_new_mapping()` + `leb_commit_mapping_swap()` is that same algorithm
one layer down, and UBI is already paying for it: payload to a PEB off the free
pool, VID header as the commit point, atomic EBA swap. A failure before the swap
leaves the previous content intact — which is exactly `compact_commit()`'s
guarantee, obtained for one erase (the recycled PEB, reclaimed inline) instead of
five.

| | today | via atomic replace |
|---|--:|--:|
| erases per compaction | 5 | **1** |
| erases per 10 000-person fill | **4 924** (measured) | ~985 |
| erase time in that fill, at ~1 072 ms | ~88 min | **~18 min** |
| ≈2.2 h fill becomes | — | **≈1.2 h** |

Deleted along with it: the scratch sector, the seal record, the `COMPACTING`
master state, `recover_compaction()`, and the `COMPACT_OR_WEDGE` path — the
"atomic window that cannot be safely aborted" stops existing, because UBI's
mapping swap *is* the atomic point.

This is the one lever that attacks a cost the flash genuinely imposes. Per
`persondb-case-performance.md` §3.2a, the 65 µs transaction constant is ~25× the
QSPI bus time for a read command and is therefore driver overhead; the ~1 072 ms
erase is the MX25R6435F itself.

---

## 3. The blocker: `data_size` makes a replaced LEB unappendable

**This is the finding that shapes the porting interface, and it is not obvious
from the API.**

`ubi_leb_write()` records `vid_hdr.data_size = len`. `ubi_plain_leb_read()` then
bounds every subsequent read to it:

```c
size_t read_bound = vid_hdr.data_size;
if (read_bound == 0) read_bound = usable_leb_size;   /* 0 == "whole LEB" */
if ((offset + len) > read_bound) return -EINVAL;
```

`blob_db` compacts a bucket **and then keeps appending to it**. So the naive
port — compact via `ubi_leb_write(lnum, image, image_len)` — sets
`data_size = image_len`, the appends land fine (`write_at` does not check), and
the *next slot-header read past `image_len` fails with `-EINVAL`.* A store that
mounts, fills, compacts once, and then reads back garbage-free right up until it
doesn't.

So the seam cannot be "replace this block". It must be **"replace this block and
leave it appendable"**, i.e. commit with `data_size = 0`. That is one parameter
in UBI, or one variant call. It has to be decided before any of §4 is written.

---

## 4. The porting interface, minimally expanded

No duplicated implementation is needed for §2. One capability, one call, one
branch:

```c
/* blob_db_store.h */
struct blob_db_store_caps {
        bool atomic_replace;   /* replace is atomic w.r.t. power failure */
};
void blob_db_store_caps_get(struct blob_db_store_caps *out);

/* Replace the whole block at @off with @len bytes, atomically if caps say so.
 * The block MUST remain appendable afterwards — see §3. */
int blob_db_store_replace(off_t off, const void *buf, size_t len);
```

- **`flash_area`**: `caps.atomic_replace = false`; `replace` is today's
  erase-then-write; `compact_commit()` keeps the five-erase protocol that
  supplies the atomicity the backend cannot.
- **UBI**: `caps.atomic_replace = true`; `replace` is `ubi_leb_write()` with
  `data_size` cleared.
- `compact_commit()` branches once, at the top.

Everything above the seam is unchanged, both formats stay readable by their own
backend, and the flash_area path does not move a byte.

---

## 5. What a duplicated implementation would buy — and why not yet

Cheap atomic replace *enables* a different data structure: drop the append log
and rewrite the block in place as an indexed record array. A lookup becomes
read-index + read-record — **2 transactions instead of ~64.**

Cost it before wanting it. Rewriting a 64 KB LEB to change a 400 B record is
160× write amplification: one erase (~1 072 ms) plus 64 KB of program (~16 ms at
the 4 MB/s bus) *per write*, against today's ~0.5 amortised erases per write.
Writes get roughly 2× worse; reads get ~30× better.

For a write-once-read-forever product that can be the right trade. But **the
same read win is available without UBI and without the write penalty**, so
duplicating the implementation buys it a second time at a price the cheaper
option does not pay:

**A two-ended log.** Slots grow up from the head of the sector as today; a
fixed-size index entry — `{ id:8, offset:2, len:2 }` = 12 B — is appended
*downward* from the tail on every slot write. Both regions are append-only, so
no erase and no rewrite: this works on raw NOR, unchanged, and on UBI.

A lookup then reads the index region in **one** transaction instead of walking
slot headers. Measured occupancy is 64.4 slots/sector, so the index is ~773 B:

| per `blob_db` get | today | two-ended log |
|---|--:|--:|
| transactions | ~66 | **~3** |
| modelled cost | 66 × 65.5 µs = 4.3 ms | 65.5 + 773 × 0.63 = **0.55 ms** |

Applied to persondb's `check` (4 `blob_db` gets): **264 transactions → ~10**,
and the modelled decision falls from 25.4 ms to ≈10.8 ms — with the residue now
dominated by *bytes*, which is what §4 of the companion document attacks.

So: **the erase win is UBI's, the transaction win is not.** They compose, and
neither requires the other.

---

## 6. What the two together are worth

| | today | atomic replace | two-ended log | both |
|---|--:|--:|--:|--:|
| erases per fill | 4 924 | **985** | 4 924 | **985** |
| fill (modelled) | ≈2.2 h | **≈1.2 h** | ≈2.2 h | **≈1.2 h** |
| `check` transactions | 264 | 264 | **~10** | **~10** |
| `check` (modelled) | 25.4 ms | 25.4 ms | **≈10.8 ms** | **≈10.8 ms** |

Both are on-flash format changes, so both cost a reformat — but only one
reformat if they land together.

---

## 7. Order of work, and what to measure before committing to any of it

1. **Profile the 65 µs transaction cost.** §3.2a of the companion document shows
   it is ~25× the QSPI bus time for a read command, so it is driver overhead, not
   flash. If it is reducible, everything transaction-shaped in §5 reprices
   downward and may not be worth a format change at all. This is the cheapest
   experiment here and it gates the most expensive work.
2. **Remove UBI's per-read VID-header read** (`persondb-case-performance.md`
   §12.2a). It adds a fixed 32 B and one transaction to *every* read, against a
   12 B `slot_head` — so it costs +82 % on the read path today and **+64 % even
   if the driver overhead in §3.2a is fixed**, because the byte ratio is worse
   than the transaction ratio. Either cache `data_size` in the EBA node (~8 B
   per LEB, immutable for the life of a mapping) or drop the bound for dynamic
   volumes and match Linux, which deletes the read instead of caching it. Until
   one of them lands, UBI is the slower backend on reads — which is most of what
   a provisioned access-control store ever does, and enough to cancel the erase
   win above.
3. **Prototype `blob_db_store_replace` on UBI** and count erases across a fill.
   One build, one run, and §2's 5× is either there or it is not.
4. **Confirm the free-pool depth holds.** `BLOB_DB_UBI_SPARE_PEBS` is 4, and
   every atomic replace consumes a free PEB and triggers an inline reclaim erase
   to refill it. At 985 compactions per fill that path is exercised hard, and it
   is where a "1 erase per compaction" claim would quietly become two.
5. Only then decide between the seam change alone (§4) and the structural change
   (§5).

---

## 8. Limits of this analysis

- **Nothing here is implemented or measured as proposed.** The erase counts and
  slot occupancy are measured; every "after" figure is arithmetic on them.
- The **1 072 ms erase** and the fitted read constants come from the DK
  (`app_perf/RESULTS.md`); the counters come from `native_sim`. That is the same
  split `RESULTS.md` §1 sanctions — structure transfers, wall-clock does not.
- §5's two-ended log is a sketch, not a design. It has not been costed for
  crash safety (the index entry and its slot are two writes, so the ordering
  question F5 asks of applications reappears inside `blob_db`), nor for what
  happens when the index region and the slot region meet.
- The Linux-UBI comparisons in the companion document are from knowledge, not
  from kernel source.
