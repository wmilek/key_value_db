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

**The write side moves further than the read side, and by more.** K5 rewrites a
directory whenever a bucket is touched for the first time. Over a full fill:

| | directory-rewrite traffic |
|---|--:|
| one level | 2 047 × 16 384 B = **32.0 MiB** |
| two levels | 2 047 × 376 B = **752 KiB** + one 368 B top-level write |

**44× less**, on a medium whose erase blocks are the scarce resource and whose
compaction cost is what caps the store (K13). This is a bigger effect than the
7× on reads and was missing from the first draft of this proposal.

### 5.1 Margin against K2 stops being expensive

This is the result that decides whether v1 is enough on its own, so it belongs
beside the byte counts rather than in the discussion.

In a one-level map, buying safety against K2 means more buckets, and more
buckets means a proportionally larger directory read on **every** operation. In
a two-level map the directory grows as √N while the buckets shrink as 1/N, so
over-provisioning is free — and for a while it is better than free:

| buckets | one level: bytes per `get` | two levels: bytes per `get` |
|--:|--:|--:|
| 2 070 (≈ what this app needs) | 18 076 B | 2 418 B |
| 4 096 (2× margin) | 33 606 B — and **unbuildable**, K12 | 1 886 B |
| 8 100 (4× margin) | — | **1 884 B** |
| 16 384 (8× margin) | — | 2 275 B |

**Eight times more buckets than the population needs costs nothing**, and four
times is the cheapest point on the curve. The same margin in a one-level map
doubles the cost of every lookup and, past 2×, cannot be built at all.

That changes what K2 means in practice. The reason `DESIGN.md` §6.1 enumerates
a whole population offline, and the reason it targets 28 % of a bucket ceiling
rather than 60 %, is that margin is expensive and being wrong is unrecoverable.
Two levels removes the first half of that. It does not remove the second — see
§9.1 and D4.

### 5.2 The objective is write bytes, not read bytes — so size for small buckets

§5.1 prices margin by what a `get` moves. That is the wrong cost to optimise
against, and correcting it moves the answer.

A write is erase-and-program, it leaves the superseded copy behind as garbage,
and that garbage must be compacted — the same compaction whose granularity caps
the store at 9 670 persons (**K13**). A read moves bytes once and costs nothing
afterwards. So the objective is bytes *written*, and **K4 sets it: every `set`
rewrites the whole bucket blob.** Write cost per operation is bucket size,
directly.

Inserting *k* entries into one bucket rewrites it *k* times, at 1e, 2e … ke, so
a fill writes about `E·e·(k+1)/2` with `k = E/N` — **inversely proportional to
the bucket count.** Modelled over this application's people map (8 000 entries,
433 B mean):

| buckets | entries/bucket | mean bucket | **bytes written over the fill** | bytes per `get` |
|--:|--:|--:|--:|--:|
| 529 | 15.1 | 6 548 B | 26.7 MiB | 6 932 B |
| 2 070 | 3.9 | 1 673 B | 8.1 MiB | 2 417 B |
| 4 096 | 1.9 | 846 B | 5.0 MiB | 1 886 B |
| **8 100** | **1.0** | **428 B** | **3.4 MiB** | **1 884 B** |
| **16 384** | **0.5** | **211 B** | **2.6 MiB** | 2 275 B |
| 65 025 | 0.1 | 53 B | 2.0 MiB | 4 149 B |

*(Analytic, from K4's rewrite rule and the enumerated population — not measured.
`CONFIG_BLOB_DB_IOSTATS` can check it, and D5 asks for that.)*

**The write curve keeps falling past the read optimum.** From 8 100 to 16 384
buckets, writes fall 24 % while reads rise 21 % — and those two percentages are
not worth the same. Smaller blobs also compact more finely and strand less of
the medium, which is the K13 mechanism running in the favourable direction, so
the same choice buys capacity as well.

**The design rule that falls out is roughly one entry per bucket** — not a
comfortably full bucket. That is the opposite of what a one-level map forces,
where few buckets is the only affordable choice and each therefore holds many
entries and is rewritten whole on every touch.

**The floor is per-blob overhead**, and it is what stops "more buckets" from
being free forever: a 14 B slot header on a 211 B bucket is 7 %, on a 53 B
bucket 26 %, plus a blob id and bookkeeping each. Somewhere below a few hundred
bytes per bucket the overhead wins. For this application the useful band is
**8 100–16 384 buckets**, and the curve is flat enough across it that the choice
is not delicate — which is §5.1's point restated on the axis that matters.

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

**This is not a redesign. `kvhash` does not conform to the contract it already
has.** The field is documented as an entry count in two headers and used as a
bucket count in the implementation:

| | says |
|---|---|
| `shape_map.h:40` | `size_t initial_capacity; /**< expected entry count (0 = provider default) */` |
| `kvdb.h:74` | `size_t initial_capacity; /**< entry-count hint (0 = default) */` |
| `l2_containers.md:171` | "`nbuckets` comes from `map_config.initial_capacity` at `create`" |
| `kvhash.c:100` | `size_t n = initial_capacity ? initial_capacity : DEFAULT_BUCKETS;` |

The two API headers promise an entry count. The L2 contract document says it is
the bucket count. The implementation uses it as the bucket count and clamps it
to `(MAX_PAYLOAD − 8) / 8`. **The documents disagree with each other**, and the
code follows the one a caller is least likely to read.

So the plain reading — "I have 8 000 entries", which is what both headers invite
— asks for 8 000 buckets, is clamped to the maximum, and produces the most
expensive map the geometry can build. `app_cbor_persondb` passes `SIZE_MAX`
instead, meaning "the largest you can build", and at a 32 722 B payload that is
a map whose directory owns an erase block and whose fill dies at person 36
(**K12**). Both readings are defensible and both are traps.

**And the exposure is not this application's.** `kvdb_create` passes the field
straight through (`kvdb.c:100`), so every `kvdb` caller who reads its own header
and passes an entry count is sizing a bucket directory without knowing it. This
is **K9** — "requested capacity is reinterpreted" — with the contract text now
showing which way round the reinterpretation goes.

Fixing D3a therefore is not "change the API". It is choosing which of two
existing, contradictory specifications the code should honour. Declaring the
population is the reading both headers already give a caller.

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
geometry it already knows — **targeting roughly one entry per bucket** (§5.2),
not a comfortably full one, because write cost is bucket size and a fill's write
traffic is inversely proportional to the bucket count. This is not a hypothetical improvement: **every
number in `app_cbor_persondb`'s `DESIGN.md` §6.1, and every line of
`tools/sizing.py`, is computable from that triple.** The application performs
the container's arithmetic offline today only because the interface will not
take the inputs.

### 7.1 `{0}` means "I do not know", and that is a contract, not a convention

The zero value has to be defined before the fields are, because it decides what
happens in the common case where an application genuinely cannot answer.

**The rule:**

- **`cfg == NULL` and `cfg == {0}` are identical**, and both mean *no
  information supplied*. The container chooses everything. This is never an
  error.
- **Each field is independently unset at zero.** Partial knowledge is the normal
  case, not a special one: an application that knows it will store roughly
  20 000 credentials but has no view on their size sets `expected_entries` and
  leaves the rest zero, and the container defaults only what it was not told.
- **A field that *is* set is a request the container must honour or refuse** —
  `-ENOSPC` or `-EINVAL`, never a silent clamp.

That last clause is what makes the convention worth writing down, because it
resolves **K9(b)** as well as K9(a). Today `buckets_for()` clamps every request
to `MAX_BUCKETS` and returns success, so "384 buckets" quietly becomes 127 and
the caller is never told (`app_perf_kvdb` does exactly this in-tree). Under this
rule the clamp has nowhere to hide: *unset* is the container's business, *set*
is the caller's, and silently delivering something else than what was asked is
not one of the two.

It also removes the `initial_capacity = SIZE_MAX` idiom, and should.
`app_cbor_persondb` passes it to mean "the largest you can build" — an intent
the current API has no word for except an out-of-range number that the clamp
converts into a value. Under the rule above `SIZE_MAX` is a request for
`SIZE_MAX` entries and must fail. The application that wanted "as large as
possible" turns out to have wanted the wrong thing anyway (§7, K12), and what it
actually needed — "I have no strong opinion, size this sensibly" — is `{0}`.

**Forward compatibility comes free**, which matters for a struct that this
proposal is already extending once: a caller compiled against a later header
that adds a field keeps working, because the field it never heard of is zero and
zero means unset.

**The honest difficulty is what the container does with `{0}`.** It must pick,
and any pick is wrong for someone. Today it picks `DEFAULT_BUCKETS = 8` — a map
that overflows at about ninety person-sized entries (K9). Two levels change the
economics of that choice, because §5.1 and §5.2 make a generous default nearly
free on reads and *cheaper* on writes, so the default can be far more generous
than 8 without punishing the small-map case on lookup cost.

What it cannot escape is that **eager sub-map creation makes a generous default
cost blobs at `create`**: 64 × 64 buckets is 64 sub-maps written up front, which
is right for a database and absurd for a three-key boot-counter. So the zero
config is exactly where v1's lack of promotion (D4) bites hardest — a declared
population lets the container size correctly, and an undeclared one leaves it
guessing with no way to correct itself later. Two mitigations, neither free:

- Keep `{0}` **one-level with a modest bucket count**, close to today's
  behaviour, and treat two levels as something a declared population opts into.
  Cheap, and leaves undeclared maps no better off than they are now.
- Let `{0}` mean *two levels, small fan-out* — a floor high enough to survive an
  unexpected population without paying much when unused.

This proposal recommends the first for v1 and the second once splitting exists,
since splitting is what makes an initially small guess recoverable. **D3e asks
review to settle it**, because it is the difference between a default that is
merely defined and one that is safe.

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

### 7.2 Per-map bucket sizing

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

**9.1 Crash consistency is a property of the split path, not of the second
level.** An earlier draft of this section said crash consistency was "the real
work" of Option B. That was wrong, and the correction changes the staging of the
whole proposal.

**A fixed two-level map has no new crash exposure at all.** The top level holds
*m* sub-map root ids. If the sub-maps are created eagerly at `create`, the top
directory is written **once** and never again — every subsequent `set` touches a
leaf bucket and, on that bucket's first touch, its own sub-map's directory. The
top level is exactly as stable as `app_cbor_persondb`'s superblock, which is
written once and read at boot.

The multi-blob `create` is **B8**'s existing window, hit once per store. It is
the same window `persondb`'s `create_store()` already has today — allocate the
roots, create the maps, bind the superblock last. Not a new risk: the same risk,
in the same place, at the same frequency.

**Eager creation is therefore a design requirement, and it is cheap.** Creating
sub-maps lazily would rewrite the top directory once per sub-map — K5 one level
up. Eager creation costs 45 directories of 376 B, about 16.9 KB and ~2.9 ms at
65 µs per transaction, once in the life of the store.

**What actually carries the risk is splitting.** A split allocates a sub-map,
rehashes entries into it, and repoints a top-level slot — several blobs, no
multi-blob atomicity, and B8's window moves from once per store to once per
split. §2.2's stage/prepare/commit protocol is the intended answer; whether it
covers a split without a reachability GC is what review must settle.

**So the two halves stage cleanly, and should:**

| | retires | crash-consistency work |
|---|---|---|
| **v1** fixed two levels, eager sub-maps | K1, K5, K11, K12, K13 | **none beyond today** |
| **v2** split on overflow | K2, K3, and the sizing ritual | the whole of it |

**v1 is the decision (D4).** It is close to mechanical, delivers every
byte-count and capacity result in §1 and §5, and leaves K2 and K3 open — which
§5.1 argues is affordable, because two levels make the margin that guards
against K2 essentially free.

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

**9.5 RAM is unchanged — an earlier draft of this section was wrong.** It
claimed two directory buffers instead of one. Walking the access path shows one
still suffices:

- `get`: read the top directory into `dir_buf`, copy out the 8-byte child root,
  then reuse the **same** `dir_buf` for the sub-directory. The top level is not
  needed again.
- `set`: identical, and the directory that may need rewriting is the
  sub-directory — which is what `dir_buf` holds at that point, because the top
  level is never modified after `create` (§9.1).

So `dir_buf` and `bkt_buf` stay as they are, at `MAX_PAYLOAD` each. **The cost
is one extra blob read per operation, not extra RAM.** Both levels share the
directory format and the same `dir_load()` path, which is what "uniform levels"
(§8) buys in code as well as in format.

Caching the top level remains optional and is where K7 (file-scope buffers
shared across all maps) and R1 (O(1) steady-state RAM) would start to bind — but
v1 does not need it.

## 10. Recommendation

**Option B, implemented so that C stays reachable**, and A folded in as the
leaf-level id scheme if `blob_db` gains the range call.

**Ship v1 only** (D4, decided): a fixed two-level map with eagerly created
sub-maps, no splitting. It carries no crash-consistency work beyond what the
stack already does once per store, and it delivers every number in §1 and §5.

K2 and K3 stay open as findings, and §5.1 is why that is acceptable rather than
a compromise: two levels make bucket margin free, so the sizing question stops
being a calculation an application can get fatally wrong and becomes a generous
default. Splitting is deferred until an application needs a genuinely unbounded
population, or until B8 gains a multi-blob commit.

**Size for small buckets, not for full ones** (§5.2). The generous default is
not only safe, it is *cheaper on the axis that costs most*: writes are
erase-and-program plus the compaction that follows, K4 rewrites a whole bucket
on every `set`, and fill write traffic is inversely proportional to the bucket
count. Roughly one entry per bucket, with per-blob overhead as the floor.

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

## 11. What blocks implementation

D4 settled the shape. What remains is not design: it is one missing piece of
test infrastructure, one unspecified detail, and two decisions. Listed because
"the design is agreed" and "someone can start" are not the same state.

### 11.1 `kvhash` has no test suite — prerequisite, not a nicety

`tests/lib/containers/` is a skeleton: a README saying "planned but not yet
implemented", no `testcase.yaml`, so `west twister` skips the directory. The
only coverage `kvhash` has is indirect, through `tests/lib/kvdb`.

This proposal rewrites an on-flash format. Doing that against no direct contract
tests is the largest practical risk in the plan, and it is larger than anything
in §9. The suite has to exist first, and it has to cover at minimum:

- every `map_ops` entry point against the §4.3 contract, including the error
  returns (`-ENOSPC`, `-ENOENT`, `-EEXIST`) that callers branch on;
- persistence across a remount, since the format is the thing changing;
- the K2 boundary — a bucket taken to the payload ceiling — because that is the
  failure this design is meant to make cheap to avoid, and it must keep
  behaving identically in v1, which does not retire it;
- key distribution across both levels (§11.2).

Writing it against the *current* implementation first is worth the extra step:
it establishes that the tests describe the contract rather than the new code,
and it gives a before/after on identical assertions.

### 11.2 Two independent indices from one key — unspecified

Today there is one hash and one index: `fnv1a(key, klen) % n`
(`kvhash.c:225`). Two levels need a top index and a sub index that do not
correlate. Naive `h % m` and `h % n` from a single 32-bit `h` cluster keys
whenever *m* and *n* share factors — and clustering is precisely what **K2**
punishes, on a structure whose whole argument is that it makes margin cheap.

Options, in the order this proposal would try them: split a 64-bit hash into
disjoint bit ranges; or salt `fnv1a` differently per level; or mix the level
index into the seed. Any of them is fine; leaving it to chance is not.

Worth noting that `app_cbor_persondb`'s hand-built two-level version avoided
this **by accident** — it hashed *different inputs* at each level, `fnv1a` over
the person-id bytes to pick a shard and `fnv1a` over the key string to pick a
bucket (`tools/sizing.py`). A container hashing one key twice does not get that
for free.

This needs specifying in the contract and a distribution test in §11.1's suite.

### 11.3 The two decisions that gate code

**D3a (config shape)** decides the interface and, with it, whether v1 needs a
promote path at all: if the container is told the expected population at
`create`, depth is a create-time choice and there is no one-level → two-level
transition to build. §7 argues this is choosing between two contradictory
existing specifications rather than inventing an API.

**D1 (contract revision or second provider)** sets the module name, the Kconfig
symbol, whether existing stores break, and whether `kvdb` needs a translation
where it passes `initial_capacity` through (`kvdb.c:100`). Cheap to decide,
but it precedes the first commit.

### 11.4 What D4 already cleared

For the record, so these are not re-opened as blockers:

| | why it is not blocking v1 |
|---|---|
| **D2** `alloc_id_range()` | v1 stores child ids in directories; Option A is not on the v1 path |
| **D3** promotion | disappears if D3a is declarative — depth is chosen at `create` |
| **D3c** per-map bucket sizing | redundant once the container derives bucket size from a declared population (§7.2) |
| p99 write measurement | moves to v2 with the splitting it was meant to characterise |
| RAM for a second directory | not needed — one `dir_buf` suffices (§9.5) |

## 12. Decisions this proposal needs from review

- **D0. The `kvhash` test suite is a prerequisite** (§11.1). `tests/lib/
  containers/` is an empty skeleton and the only coverage is indirect via
  `tests/lib/kvdb`. Is writing it — against the current implementation first —
  accepted as the first commit of this work rather than part of it?
- **D1.** Is the bucket-id array leaving the `kvhash` contract (§4.3), or is
  this a second provider (`kvhash2`) beside it? (§11.3)
- **D2.** Does `blob_db` gain `alloc_id_range()`? If not, Option A and Option C
  are both off the table and B is the only route.
- **D3.** Fixed two-level, or one-level promoted on growth? §8 answers the
  shape: **uniform levels with depth in the directory header, promoted, shipped
  at two.** What review must confirm is that promotion is in v1 rather than
  deferred — a fixed two-level map is cheaper to build and forecloses the third
  level (§8) and the small-map case (§9.3).
- **D3a.** Does `map_config` become declarative (§7) — `expected_entries`,
  `typical_entry_bytes`, `max_entry_bytes` — or does `initial_capacity` stay and
  merely get documented as the bucket count it is? Note this is **not** a choice
  about whether to change an API: `shape_map.h` and `kvdb.h` already document
  the field as an entry count and the implementation already treats it as a
  bucket count, so one of the two is being corrected either way (§7).
  Declarative is what lets `tools/sizing.py` be deleted; keeping the current
  field means every application keeps doing that arithmetic offline, and every
  `kvdb` caller keeps reading "entry-count hint" and getting a bucket count.
- **D3d.** How are the two level indices derived from one key (§11.2)? Split a
  64-bit hash, or salt per level? Unspecified today, and getting it wrong
  clusters keys into exactly the failure K2 punishes.
- **D3e.** What does `{0}` build (§7.1)? The zero config — `NULL` or all-zero,
  meaning "I do not know" — must map to *something*, and today that something is
  8 buckets, which overflows at about ninety person-sized entries. One level
  with a modest count (close to today, and no worse), or two levels with a small
  fan-out (survives an unexpected population, costs sub-map blobs at `create`
  even when unused)? Recommended: the first for v1, the second once splitting
  makes an initially small guess recoverable.

  The rest of §7.1's rule is not offered as a choice, because it is what makes
  the field meanings enforceable: `NULL` ≡ `{0}` ≡ no information; each field
  independently unset at zero; and **a field that is set is honoured or
  refused, never silently clamped** — which closes K9(b) as well as K9(a).
- **D3b.** Is `kvhash_info()` (readback of depth, fan-out, bucket count, entry
  count) in scope? It closes half of **K9** and much of **K10**, and it is
  independent of everything else here.
- **D3c.** Per-map bucket sizing (§7.1) — worth it, or does declarative config
  make it redundant by deriving bucket size from the declared entry size?
- **D4. DECIDED: v1 — fixed two levels, splitting deferred.**

  Rationale, recorded because the decision closes the proposal's largest open
  question: v1 carries **no crash-consistency work beyond what the stack already
  does once per store** (§9.1), and it delivers every result in §1 and §5 — 7×
  on reads, 44× on directory-rewrite traffic, and the end of K13's capacity
  wall. Splitting carries all of the risk and none of those numbers.

  **What v1 does not do, stated plainly.** K2 and K3 remain open as findings. A
  bucket can still burst, the bucket count is still fixed at create, and an
  application whose population grows past what it declared still has no recovery
  short of a reformat.

  **Why that is tolerable here, and what makes it so.** §5.1 is the load-bearing
  reason: in a two-level map, 8× more buckets than the population needs costs
  nothing per lookup, and 4× is cheaper than sizing exactly. Margin against K2
  becomes free. K2 stops dictating the design even though it is not retired —
  the sizing question turns from "compute the tail correctly or lose the fill"
  into "declare generously and stop thinking about it".

  **v2 is deferred, not dropped.** Revisit it when an application appears whose
  population is genuinely unbounded at create time, or when B8 gains a
  multi-blob commit that makes a split cheap to make correct.

- **D5.** What is the acceptance measurement? For v1, in order of how much each
  one proves:

  1. **`app_cbor_persondb` deletes `tools/sizing.py`** and passes its two
     populations to `map_config` instead (§7). Still the sharpest criterion,
     and still reachable without splitting — not because the container can prove
     its sizing is right, but because §5.1 makes a generous default affordable,
     which is what the offline enumeration was buying.
  2. **10 000 persons completes** — no configuration currently reaches it
     (**K13**), and the benchmark had to be re-sized to 8 000 because of it
     (`DESIGN.md` §6.5). This is the direct test of whether the capacity wall
     is gone.
  3. **Total bytes written over a full fill**, with `CONFIG_BLOB_DB_IOSTATS`.
     Two components, both modelled and neither yet measured:
     directory-rewrite traffic should fall from **32.0 MiB to under 1 MiB**
     (§5), and bucket-rewrite traffic from ~8 MiB to **~3 MiB** at one entry per
     bucket (§5.2). The write side is where this design earns its keep, and it
     is the part of the proposal resting on a model rather than a measurement —
     so it is the number to check first.

  The p99-write measurement moves to v2 with the splitting it was meant to
  characterise.
