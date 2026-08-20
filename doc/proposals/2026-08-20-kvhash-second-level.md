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
| mean bucket | 1 486 B |
| **bytes moved per `get`** | **17 870 B** |
| of which the app actually wanted | 433 B |

Measured on `native_sim`, 200 samples, steady state (`RESULTS.md` §4, "Two
`kvhash` instances", at the re-sized 8 000 persons): a `check` — card → person
→ permission, two map gets — costs **179 µs** and moves **37.4 KB** of flash, an
amplification of **93×**. A `miss` moves 18.0 KB to discover that a 23-byte key
is absent: **782×**.

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

All figures below are `app_cbor_persondb` at its re-sized 8 000 persons
(`DESIGN.md` §6.5), 2 047 buckets, mean bucket 1 486 B:

| | root/dir bytes per `get` | + bucket | **total** | largest blob |
|---|--:|--:|--:|--:|
| today, one level | 16 384 | 1 486 | **17 870 B** | 16 384 B |
| two levels, 45 × 46 | 368 + 376 | 1 486 | **2 230 B** | 4 621 B |
| option A, one level | 16 | 1 486 | **1 502 B** | 4 621 B |

Metadata per operation drops from O(N) to O(√N). A third level would give
O(N^⅓), which is not worth a transaction at this scale but is worth 2× at
~65 000 buckets — §8 bounds when it matters and what that implies for the
format.

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

## 7. The create-time configuration is part of this

A structure that sizes itself is only reachable through an interface that
accepts the facts it needs. Today's does not.

**The defect is that `initial_capacity` is a bucket count wearing the name of a
capacity.** `buckets_for()` takes it as the bucket count and clamps it to
`(MAX_PAYLOAD − 8) / 8`. So the plain reading — "I have 8 000 entries" — asks
for 8 000 buckets, is clamped to the maximum, and produces the most expensive
map the geometry can build. `app_cbor_persondb` passes `SIZE_MAX` instead,
meaning "the largest you can build", and at a 32 722 B payload that is a map
whose directory owns an erase block and whose fill dies at person 36 (**K12**).
Both readings are defensible and both are traps.

**Rejected: per-level fields.** The obvious extension —
`initial_capacity_L1`, `initial_capacity_L2` — is the current mistake
multiplied. It makes the caller state the container's internal geometry, which
is **X1** ("an application cannot ask the stack anything, so it restates it") as
a struct member; it triples **K9**'s reinterpret-and-clamp surface; and if depth
is promoted on growth (§8) then at create time there is no L2, so the field is
either ignored or a lie that changes meaning when the map promotes.

**Proposed: declare the population, not the geometry.**

```c
struct map_config {
        size_t expected_entries;      /* 8 000 persons; 20 000 credentials */
        size_t typical_entry_bytes;   /* 433 B;           23 B             */
        size_t max_entry_bytes;       /* the tail is what bursts, not the mean */
};
```

`kvhash` derives fan-out, depth and bucket size from those three plus the
geometry it already knows. This is not a hypothetical improvement: **every
number in `app_cbor_persondb`'s `DESIGN.md` §6.1, and every line of
`tools/sizing.py`, is computable from that triple.** The application performs
the container's arithmetic offline today only because the interface will not
take the inputs.

`max_entry_bytes` is load-bearing, not padding. **K2 is about variance, not the
mean** — a bucket bursts on the tail. Sizing from a mean alone is exactly how
the analytic estimate failed at person 9 232 and why §6.1 had to switch to
enumerating the whole population.

Two more pieces make it a contract rather than a hint:

- **Readback.** `kvhash_info()` reporting actual depth, fan-out, bucket count
  and entry count. That closes the "cannot be read back" half of **K9** and much
  of **K10** — `app_cbor_persondb`'s store report currently prints "bucket count
  not observable — FINDINGS.md K10" where the number should be.
- **An override, named as one.** `bucket_count_override` for the caller who
  genuinely knows better, kept out of the path a naive caller reaches for first.

**What this does not fix.** Declarative config makes the *first* guess right; it
cannot help a population that outgrows what was declared, because `n_buckets` is
fixed after create (**K3**). Only splitting makes being wrong survivable. The
two are complementary, and neither substitutes for the other.

### 7.1 Per-map bucket sizing

A related question, and the answer bounds it: can bucket *size* be per-map
rather than one global `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`?

**Downward, yes.** A per-map ceiling is implementable — `blob_db` accepts any
blob within its own limit, the directory header already carries `n_buckets`, and
§4.3 reserves a `version` byte. **Upward, no.** Three things pin the global
symbol: `blob_db`'s rebind rule `(sector − 16) / 2 − 14`, the file-scope
`dir_buf`/`bkt_buf` sized at `MAX_PAYLOAD` (**K7**), and `append_slot`'s
`MAX_PAYLOAD + 46` stack frame (**B5** job 3). Per-map config carves within a
global ceiling; it cannot raise one. So it does **not** fix K13 — a map wanting
2 047 buckets still has a 16 384 B directory.

What it does fix is one map paying for another's geometry, and there is a
measured case. `app_cbor_persondb`'s credential map holds 19 969 entries of 23 B
and is given 2 047 buckets, because the app asks both its maps for the largest
available:

| credential-map buckets | directory | mean bucket | bytes per `get` |
|--:|--:|--:|--:|
| 255 | 2 048 B | 1 801 B | **3 849 B** |
| 511 | 4 096 B | 899 B | 4 995 B |
| 2 047 *(today)* | 16 384 B | 224 B | **16 608 B** |

A 16 384 B directory to index buckets averaging 224 B — **4.3× waste on the
credential half of every access decision**, roughly 16.6 KB of the 37.4 KB an
access decision moves. Under the declarative config above the application would
not have chosen this at all: it would have declared 20 000 entries of 23 B and
been sized accordingly.

## 8. How deep? A bound on levels

Three levels would give O(N^⅓). Whether that is ever needed decides whether
depth is a parameter or a constant, so it is worth answering with numbers rather
than deferring. Cost model is `RESULTS.md` §5's — 65 µs per flash transaction
plus 0.63 µs/B — with a 1 486 B bucket:

| buckets | store | k=1 | k=2 | k=3 |
|--:|--:|--:|--:|--:|
| 2 047 | ~3 MB (this app) | 11 388 µs | **1 595 µs** | 1 408 µs |
| 65 025 | ~100 MB | 328 797 µs | 3 712 µs | **1 816 µs** |
| 261 121 | ~390 MB | — | 6 292 µs | **2 179 µs** |

**Capacity never forces a third level on this class of hardware.** Two levels
with a 2 KB directory budget address 65 025 buckets ≈ 97 MB; with 4 KB,
~392 MB. The DK's part is 8 MB and needs about 5 000 buckets — 12× to 80×
headroom. The `u16 n_buckets` in the format allows 65 535 per level, so the
format is nowhere near binding either.

**Latency eventually does.** At this application's scale a third level saves
12 %, which is inside the noise and not worth a transaction. At ~65 000 buckets
it is 2×. So the crossover is a store in the **tens of megabytes** — past this
stack's current target, but ordinary for QSPI NAND or a larger NOR part.
"Never" is the wrong word.

Two things argue against building it now:

- **A RAM-cached top level buys most of k=3's benefit.** At fan-out 255 the top
  directory is ~2 KB; holding it removes a transaction and most of the metadata
  bytes. Not free — R1 (O(1) steady-state RAM) and **B6** bound it — but a
  cheaper lever than a level, and it scales with open maps rather than with
  data.
- **Each level is another blob to keep consistent across a split**, which §9.1
  already names as the hard part.

**So the conclusion is a constraint, not a level count: make the levels uniform
and put depth in the directory header.** Then 2 → 3 is a version bump plus the
promote path that 1 → 2 already requires. If "two" is baked into the code paths,
a third level is a rewrite. Ship at two; state that three is unnecessary below
~10⁴ buckets.

## 9. Costs and open questions

**9.1 Crash consistency is the real work, not the hashing.** A split touches
several blobs and `blob_db` has no multi-blob atomicity — that is **B8**'s
leak window, currently hit once at store creation and now hit on every
split. §2.2's stage/prepare/commit protocol is the intended answer; whether
it covers a split without a reachability GC is the first thing review should
settle.

**9.2 Split latency.** Rehashing a sub-map lands in the middle of a `set`, on a
device where rewriting one bucket is already the expensive operation (K4).
A p99 write will be much worse than today's. It should be measured, not
argued about, and `app_cbor_persondb`'s fill is the workload to measure it
with.

**9.3 Two levels is overhead for a small map.** The default map is 8 buckets;
giving it a second level costs a transaction for nothing. So the map must
start one-level and grow a level, which means the promote path must be
correct from the first release.

**9.4 A format break**, guarded by the `version` byte §4.3 reserves. Existing
stores need a reformat; there is no in-place migration and none is proposed.

**9.5 RAM.** Two directory buffers instead of one, unless the top level is
cached — and K7 (file-scope buffers shared across all maps) says where that
argument leads. `blob_db` contract R1 (O(1) steady-state RAM) bounds it.

## 10. Recommendation

**Option B, implemented so that C stays reachable**, and A folded in as the
leaf-level id scheme if `blob_db` gains the range call.

The ordering that matters: A is a performance fix, B is a capability fix. A
makes this application fast; B makes `kvhash` a map rather than a fixed-size
table that the caller must size correctly in advance. Only B retires K2 and K3,
and those two are why an application has to know its population before it
creates its store — the single most limiting thing this stack asks of a product.

**And B is only half the fix without §7.** A map that sizes itself is
unreachable through an interface that asks for a bucket count: the caller would
still compute a geometry offline and hand it over, which is the ritual this is
meant to end. The declarative `map_config` is not a nicety attached to the
redesign — it is how the redesign becomes visible to an application. The
acceptance criterion in D5 is written to make that testable: if
`tools/sizing.py` survives, this did not land.

**Relationship to `kvtree` (§4.4).** The stack already specifies a multi-level
container, and it is not this one. `kvtree` is a B-tree: ordered iteration,
range scans, and ~6 reads per lookup at fan-out 8 for 100k keys. A two-level
hash is ~3. They answer different questions — if you need order, use the tree;
if you need point lookups at scale, the hash should stop being one level. This
proposal does not merge them.

## 11. Decisions this proposal needs from review

- **D1.** Is the bucket-id array leaving the `kvhash` contract (§4.3), or is
  this a second provider (`kvhash2`) beside it?
- **D2.** Does `blob_db` gain `alloc_id_range()`? If not, Option A and Option C
  are both off the table and B is the only route.
- **D3.** Fixed two-level, or one-level promoted on growth? §8 answers the
  shape: **uniform levels with depth in the directory header, promoted, shipped
  at two.** What review must confirm is that promotion is in v1 rather than
  deferred — a fixed two-level map is cheaper to build and forecloses the third
  level (§8) and the small-map case (§9.3).
- **D3a.** Does `map_config` become declarative (§7) — `expected_entries`,
  `typical_entry_bytes`, `max_entry_bytes` — or does `initial_capacity` stay and
  merely get documented as the bucket count it is? Declarative is what lets
  `tools/sizing.py` be deleted; keeping the current field means every
  application keeps doing that arithmetic offline.
- **D3b.** Is `kvhash_info()` (readback of depth, fan-out, bucket count, entry
  count) in scope? It closes half of **K9** and much of **K10**, and it is
  independent of everything else here.
- **D3c.** Per-map bucket sizing (§7.1) — worth it, or does declarative config
  make it redundant by deriving bucket size from the declared entry size?
- **D4.** Is split-on-overflow in the first revision, or does v1 ship the
  two-level layout with `-ENOSPC` retained and splitting deferred? Only the
  former retires K2.
- **D5.** What is the acceptance measurement? Proposed, in order of how much
  each one proves:

  1. **`app_cbor_persondb` deletes `tools/sizing.py`** and passes its two
     populations to `map_config` instead. This is the sharpest criterion in the
     proposal: if the application still needs offline enumeration to choose a
     safe geometry, the interface did not land, whatever the latency numbers
     say.
  2. **10 000 persons completes** — no configuration currently reaches it
     (**K13**), and the benchmark had to be re-sized to 8 000 because of it
     (`DESIGN.md` §6.5).
  3. **p99 write with splitting enabled**, measured over the fill, because
     §9.2's spike is the cost this design trades for.
