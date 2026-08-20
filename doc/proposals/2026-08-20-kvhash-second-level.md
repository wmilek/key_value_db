# Design change proposal — a second level for `kvhash`

Status: **proposal / for review** · 2026-08-20
· Target contract: `doc/layers/l2_containers.md` §4.3 (and §5 invariants)
· Target implementation: `lib/containers/kvhash/kvhash.c`
· Evidence: `app_cbor_persondb/FINDINGS.md` K1, K2, K3, K5, K10, K11, K12, K13
  · measurements in `app_cbor_persondb/RESULTS.md` §4b
· Governed by `doc/principles.md`

**The ask.** `kvhash`'s root holds one bucket id per bucket. That array is read
in full on every `get`, `set` and `del`, it is rewritten whenever a bucket is
first touched, and its size bounds the bucket count. Four separate findings and
one dead benchmark trace back to it. What should replace it?

**Short answer.** Not a bigger payload — that is what produced K12 and K13. The
array exists only because bucket ids are allocated one at a time and could be
anything. Three ways out, in increasing order of ambition and value:
**(A)** allocate the ids as a range and compute them — O(1) metadata, no growth;
**(B)** make the map two-level — O(√n) metadata, and buckets can *split*;
**(C)** linear hashing — O(1) metadata *and* growth, but it needs (A) underneath.

§8 recommends **B, built so that C remains reachable**, because only a splitting
structure retires K2 and K3, and those are what force an application to size a
map correctly before it writes a single record.

---

## 1. What the array costs, measured

From `app_cbor_persondb` at its shipped configuration — one people map,
`MAX_PAYLOAD` 16 384, so `MAX_BUCKETS = (16384 − 8)/8 = 2047`:

| | |
|---|--:|
| directory blob | **16 384 B** |
| mean bucket | 1 844 B |
| **bytes moved per `get`** | **18 228 B** |
| of which the app actually wanted | 433 B |

Measured on `native_sim`, 200 samples, steady state (`RESULTS.md` §4b): a
`check` — card → person → permission, two map gets — costs **181 µs** and moves
**37.5 KB** of flash, an amplification of **94×**. A `miss` moves 18.0 KB to
discover that a 23-byte key is absent: **782×**.

The array is 90 % of every one of those reads, and it is metadata the caller
never asked for.

## 2. Why it exists

One line, `kvhash.c:308`:

```c
if (fresh_bucket) {
        bid = blob_db_alloc_id();      /* strictly increasing, otherwise arbitrary */
        ...
        dir_set_bucket(idx, bid);      /* so it must be written down */
        rc = blob_db_update(root, dir_buf, dir_len(n));
}
```

`blob_db_alloc_id()` returns `st.next_id++` (`blob_db.c:2268`). Other blobs are
interleaved, so the ids a map receives are unpredictable, so the map remembers
them.

**The indirection is never used as indirection.** `dir_set_bucket` is called
from exactly this one place. A bucket's id is written once and never changes —
`kvhash` never relocates a bucket, because `blob_db_update` keeps the id stable
and relocates the bytes itself. The array maps slot *k* to a number that is
arbitrary only because the allocator made it arbitrary.

Note also what is *not* in the way: `nbuckets` is a `u16` on flash, so the
format already allows 65 535 buckets (K1). And §4.3 reserves the `version` byte
for evolving this layout.

## 3. What it costs beyond speed

`MAX_PAYLOAD` sets the bucket size *and*, through `(MAX_PAYLOAD − 8)/8`, the
bucket count. So the array turns one Kconfig symbol into two ceilings that move
in opposite directions, and a store's real capacity is whichever is nearer:

| `MAX_PAYLOAD` | buckets | directory | max persons | stopped by |
|--:|--:|--:|--:|---|
| 8 192 | 1 023 | 8 192 B | **11 787** | K2 — fullest bucket at **99 %** of the ceiling |
| 16 384 | 2 047 | 16 384 B | **9 670** | K13 — directory needs 16 398 B contiguous; best block has 12 032 B |
| 32 722 | 4 089 | 32 720 B | **36** | K12 — two directory copies are 65 484 B of a 65 488 B erase block |

All three measured on `native_sim` against an 8 MiB partition. The shipped
configuration reaches **9 670 persons at 49.9 % live content** — half the part
is unreachable, and the configuration that holds the most records is not the
one that is safe to ship.

## 4. Option A — range-allocated bucket ids

Reserve `base … base + n − 1` at `create`; store `base`. Then
`bucket_id(k) = base + k`.

- Root blob: 16 384 B → **~16 B**. Bytes per `get`: 18 228 → **~1 860**.
- Retires **K1** (count no longer bounded by the payload), **K5** (nothing to
  rewrite when a bucket appears), **K11**, **K12**, **K13**.
- Unwritten buckets simply have no blob: `blob_db_get` → `-ENOENT` reads as
  "empty". That also closes the crash window at `kvhash.c:319`, where a bucket
  is written but not yet published.

**Costs.** It is not purely an L2 change: `blob_db` has no range reservation, so
it needs `blob_db_alloc_id_range(n, &base)` — advance `next_id` by n, persist
the ceiling once. Small, but a contract addition, and its interaction with
**B12** (compaction lowering the durable id ceiling) must be checked. It
reserves n ids whether or not the buckets are ever written. And it makes **K3
worse**: the bucket count is not merely fixed at create, it is pre-committed.

**It does not touch K2.** A bucket that fills still returns `-ENOSPC` with no
warning, so `tools/sizing.py`, `DESIGN.md` §6.1 and the whole size-it-offline
ritual survive intact.

## 5. Option B — two levels

A top map of *m* slots, each naming a sub-map of *n* buckets. Directories are
`8 + 8m` and `8 + 8n` with `m·n = N`, minimised at `m ≈ n ≈ √N`:

| | root/dir bytes per `get` | + bucket | **total** | largest blob |
|---|--:|--:|--:|--:|
| today, 2 047 buckets | 16 384 | 1 844 | **18 228 B** | 16 384 B |
| two levels, 45 × 46 | 368 + 376 | 1 823 | **2 567 B** | 4 907 B |
| option A, one level | 16 | 1 844 | **1 860 B** | 4 907 B |

Metadata per operation drops from O(N) to O(√N). A third level would give
O(N^⅓) — 305 B here — for another transaction, which is not worth it at this
scale.

**The reason to prefer this over A is not the bytes. It is that a two-level map
can split.** Bucket overflow stops being `-ENOSPC` and becomes "split this
sub-map and rehash it". That retires **K2**, and with it the reason
`tools/sizing.py` exists: no offline enumeration of the population, no margin
chosen blind, no `-ENOSPC` nine tenths of the way through a multi-hour fill.
Combined with growth it also retires **K3**, and removes most of what **K10**
(no occupancy query) is painful *for*.

It also stays inside L2. Ids may remain arbitrary, because each level stores its
children's ids and there are few enough of them for the blob to be small.

**And it is the structure applications are already building by hand.**
`app_cbor_persondb` hashed a person id to one of sixteen maps, and `kvhash` then
hashed the key to a bucket inside it — two levels, two independent hashes, with
the top one implemented in the application (`DESIGN.md` §12.2). That is the
signal that this level belongs in the container.

## 6. Option C — linear hashing

Keep `(level, split_pointer)` in the root and compute the bucket from the hash,
splitting one bucket at a time as the map grows. Root metadata is O(1) *and* the
map grows — strictly better than A and B on paper, with no directory at all.

It needs range-allocated ids (Option A) underneath, one range per level, so it
is A + C rather than an alternative to A. It is the most speculative of the
three and should not be attempted before B's split machinery exists and is
trusted.

Note that classic **extendible** hashing is the wrong choice here: its directory
is 2^d entries, which reintroduces exactly the O(N) blob this proposal exists to
remove.

## 7. Costs and open questions

1. **Crash consistency is the real work, not the hashing.** A split touches
   several blobs and `blob_db` has no multi-blob atomicity — that is **B8**'s
   leak window, currently hit once at store creation and now hit on every
   split. §2.2's stage/prepare/commit protocol is the intended answer; whether
   it covers a split without a reachability GC is the first thing review should
   settle.
2. **Split latency.** Rehashing a sub-map lands in the middle of a `set`, on a
   device where rewriting one bucket is already the expensive operation (K4).
   A p99 write will be much worse than today's. It should be measured, not
   argued about, and `app_cbor_persondb`'s fill is the workload to measure it
   with.
3. **Two levels is overhead for a small map.** The default map is 8 buckets;
   giving it a second level costs a transaction for nothing. So the map must
   start one-level and grow a level, which means the promote path must be
   correct from the first release.
4. **A format break**, guarded by the `version` byte §4.3 reserves. Existing
   stores need a reformat; there is no in-place migration and none is proposed.
5. **RAM.** Two directory buffers instead of one, unless the top level is
   cached — and K7 (file-scope buffers shared across all maps) says where that
   argument leads. `blob_db` contract R1 (O(1) steady-state RAM) bounds it.

## 8. Recommendation

**Option B, implemented so that C stays reachable**, and A folded in as the
leaf-level id scheme if `blob_db` gains the range call.

The ordering that matters: A is a performance fix, B is a capability fix. A
makes this application fast; B makes `kvhash` a map rather than a fixed-size
table that the caller must size correctly in advance. Only B retires K2 and K3,
and those two are why an application has to know its population before it
creates its store — the single most limiting thing this stack asks of a product.

**Relationship to `kvtree` (§4.4).** The stack already specifies a multi-level
container, and it is not this one. `kvtree` is a B-tree: ordered iteration,
range scans, and ~6 reads per lookup at fan-out 8 for 100k keys. A two-level
hash is ~3. They answer different questions — if you need order, use the tree;
if you need point lookups at scale, the hash should stop being one level. This
proposal does not merge them.

## 9. Decisions this proposal needs from review

- **D1.** Is the bucket-id array leaving the `kvhash` contract (§4.3), or is
  this a second provider (`kvhash2`) beside it?
- **D2.** Does `blob_db` gain `alloc_id_range()`? If not, Option A and Option C
  are both off the table and B is the only route.
- **D3.** Fixed two-level, or one-level promoted to two on growth? (3 above.)
- **D4.** Is split-on-overflow in the first revision, or does v1 ship the
  two-level layout with `-ENOSPC` retained and splitting deferred? Only the
  former retires K2.
- **D5.** What is the acceptance measurement? Proposed: `app_cbor_persondb` at
  10 000 persons — which no configuration currently completes (K13) — plus a
  p99 write with splitting enabled.
