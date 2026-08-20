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

**What `{0}` builds — DECIDED: one level, small, exactly as today.** The
default serves the small single-level map, and a second level is something an
application asks for by declaring a population that needs one. Two consequences
worth having in writing:

- **Nothing regresses for callers who say nothing.** `{0}` and `NULL` keep
  producing today's `DEFAULT_BUCKETS` map with today's on-flash shape. A
  settings store, a boot counter, `blobfs`' directory — none of them pay a byte
  or a blob for a feature they did not ask for.
- **It removes the sharpest edge in D4.** An earlier draft of this section
  worried that v1's lack of promotion bites hardest on the undeclared map. It
  does not, under this rule: the undeclared map is *exactly as well or badly
  served as it is today*, so v1 introduces no new way to be wrong. Only a
  declared population changes anything, and a declaration is precisely the case
  where the container can size correctly.

**Where the boundary falls, and why it is not where the arithmetic first
suggests.** On lookup bytes alone two levels win almost immediately — at 64
entries a one-level directory is 520 B against 144 B for two — and the crossover
against one extra flash transaction (65 µs ≈ 103 B) lands near **25 entries**.
That is not the right threshold, because eager sub-map creation is not free:
**B7** puts a first write to a fresh `blob_db` bucket at ~1.1 s on the DK, and a
two-level map pays that per sub-map at `create`. Six sub-maps for a thirty-entry
map is several seconds of boot to save a few hundred bytes per lookup.

So the threshold is set by create cost, not lookup cost, and it sits far above
the byte crossover. The proposed rule:

```
/* Small maps: write traffic is negligible, so pack buckets and stay flat.
 * The load is capped by entry size so a bucket cannot approach K2's ceiling. */
load  = min(SMALL_MAP_LOAD, safe_bucket_bytes / max_entry_bytes)
small = ceil(expected_entries / load)

small <= ONE_LEVEL_MAX_BUCKETS
        ->  one level, `small` buckets
otherwise
        ->  two levels, ~1 entry per bucket (§5.2):
            buckets = expected_entries, m = n = ceil(sqrt(buckets))
```

**The load factor has two regimes, and conflating them was a bug in an earlier
draft of this rule.** §5.2's "~1 entry per bucket" minimises *write* traffic,
and write traffic is what dominates a store being filled with thousands of
records. It is irrelevant to a map holding a few hundred. Applying it
everywhere sent anything over `ONE_LEVEL_MAX_BUCKETS` entries to two levels —
so a 300-key settings store would have paid 28 sub-map creations at ~1.1 s each
on the DK (**B7**) to optimise write traffic it does not have.

With `SMALL_MAP_LOAD` at **4**, one level covers roughly a thousand entries:

| caller | declares | gets |
|---|---|---|
| nothing (`{0}`) | — | one level, 8 buckets — today's behaviour |
| a settings store | 50 entries | one level, 13 buckets |
| `app_perf_kvdb` | 768 entries | one level, 192 buckets |
| `app_cbor_persondb` | 8 000 persons | **two levels**, m = n ≈ 90 |

The `safe_bucket_bytes / max_entry_bytes` term is what stops the load factor
being a trap for large records: four 4 KB entries would be a 16 KB bucket, so a
map declaring those gets a lower load and more buckets instead. It is also the
only place `max_entry_bytes` does work at one level, which is why it is not
optional.

`ONE_LEVEL_MAX_BUCKETS` is a **tunable constant, not a format field**. Each map
records its own depth, so moving the threshold later changes only maps created
afterwards and never invalidates a store — which is the property that makes it
safe to pick a conservative value now and revisit it with measurements.

`max_entry_bytes` is load-bearing, not padding. **K2 is about variance, not the
mean** — a bucket bursts on the tail. Sizing from a mean alone is exactly how
the analytic estimate failed at person 9 232 and why §6.1 had to switch to
enumerating the whole population.

Two more pieces make it a contract rather than a hint:

- **Readback, under one rule: it must be free.** `map_ops.stat()` reports only
  values *already persisted because the map needs them to work*. No new header
  fields, no counters maintained on the write path, no writes of any kind. A
  diagnostic that costs a write per insert to serve a number nobody reads is
  worse than no diagnostic.

  | field | derived from | cost |
  |---|---|---|
  | `depth`, `fanout` | top-level header | 1 blob read |
  | `buckets` | `fanout × n`, `n` from any sub-directory header | 1 more read at depth 2 |
  | `entry_bytes_limit` | payload − slot and key overhead | arithmetic, no flash |
  | ~~entry count~~ | — | **excluded**: needs a counter rewritten on every insert and delete, which is the K5 traffic this proposal removes |
  | ~~largest stored entry~~ | — | **excluded**: needs a walk of every bucket (B4 is O(n²)) |

  Note `entry_bytes_limit` is the *ceiling the map can accept*, not the largest
  entry stored. It is deliberately not named `max_entry_bytes`, which is the
  input field where the application declares the largest entry it intends to
  write — the same word for a declaration and for a capability is how "expected
  entry count" came to mean a bucket count.

  **An earlier draft of this section proposed recording `n` in the top-level
  header** so `stat()` could compute `buckets` in one read instead of two. That
  is struck: it adds persisted state to every store, forever, to save one read
  in a diagnostic called once. The cost belongs to whoever calls the diagnostic.
  It would also duplicate a value that v2's splitting makes non-uniform, so the
  redundancy would have to be maintained or become wrong.

  `create(root, cfg, &info)` fills the same struct at **zero** cost, since
  `create` has every value in RAM already. That is the path an application would
  actually use; `stat()` serves reopen and reporting.

  This closes the "cannot be read back" half of **K9** and the geometry half of
  **K10** — `app_cbor_persondb`'s store report prints "bucket count not
  observable — FINDINGS.md K10" today. It does **not** close K10's entry-count
  half, and that is a deliberate trade rather than an oversight.
- **No override, and this is deliberate.** An earlier draft of this section
  proposed `bucket_count_override` "for the caller who genuinely knows better".
  That is `initial_capacity` under a friendlier name, and it is struck. It
  contradicts D5's first criterion — that an application stops computing a
  geometry — and §7.2's warning that two ways to say overlapping things is how
  the present contradiction arose. With nothing deployed there is no caller to
  preserve, so an escape hatch would preserve only the habit. If a genuine
  tuning need appears the contract can change again, which is cheap now and
  will not be later.

**What this does not fix.** Declarative config makes the *first* guess right; it
cannot help a population that outgrows what was declared, because `n_buckets` is
fixed after create (**K3**). Only splitting makes being wrong survivable. The
two are complementary, and neither substitutes for the other.

### 7.2 What a contract revision touches (D1)

Revising rather than forking is the cheaper option here, and the reason is that
the contract has one real implementation:

| | state |
|---|---|
| `kvhash` | the only implemented provider — this is the work |
| `kvlist`, `kvtree` | **9-line skeletons**, build-wired behind `default n`. Nothing to update |
| `kvdb` | passes the field through unchanged (`kvdb.c:100`) and re-exports it (`kvdb.h:74`) |
| `app_perf_kvdb:452` | asks for `N_KEYS / 2` = 384, silently gets 127 — K9(b)'s live in-tree example |
| `tests/lib/kvdb:186` | asks for 16 |
| `app_cbor_persondb` | passes `SIZE_MAX`; §7.1 removes that idiom |

Five call sites, one provider. That is what makes replacing the field
tractable rather than a migration, and it is why D1 partly settles **D3a**: with
a revision on the table, the declarative fields can *replace* `initial_capacity`
instead of coexisting with it. Coexistence would leave two ways to say
overlapping things, which is how the current contradiction arose.

**One sentence in the contract has to go, and it is not the field.**
`shape_map.h:32-38` describes `map_config` as:

> These are hints, not a contract: a container applies what is meaningful to it
> and ignores the rest (a hash uses `initial_capacity` to size its **bucket
> directory**; a linear list ignores it).

Two problems in one comment block. It says **hints, not a contract** — which is
exactly what §7.1's rule reverses: a field that is set must be honoured or
refused, never quietly ignored or clamped. And the parenthesis says the hash
uses the field to size its *bucket directory*, while the field's own doc comment
four lines below says *expected entry count*. **The struct's documentation
contradicts itself inside a single comment block**, which is the cleanest
evidence that K9(a) is a contract defect rather than an implementation bug.

The revision therefore has to state, in the shape's own words: what each field
means, that unset is the container's choice, that set is binding, and that a
provider which cannot honour a set field fails rather than proceeding.

### 7.3 Per-map bucket sizing

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

**9.1a A split breaks the uniformity that makes `stat()` free.** Noted here so
v2 inherits it rather than rediscovers it. v1's sub-maps all have the same
bucket count, so `buckets` is `fanout × n` — arithmetic over two blob reads
(§7). Splitting makes sub-maps differ by construction, after which the total is
either an O(*m*) query or a running count maintained in the top level, which
means **rewriting the top level on every split**. The top level is otherwise
written exactly once, at `create`, which is most of why v1 carries no new
crash-consistency work. So v2 pays for that number twice: once in writes, once
in the commit protocol.

**9.2 Split latency.** Rehashing a sub-map lands in the middle of a `set`, on a
device where rewriting one bucket is already the expensive operation (K4).
A p99 write will be much worse than today's. It should be measured, not
argued about, and `app_cbor_persondb`'s fill is the workload to measure it
with.

**9.3 Two levels is overhead for a small map.** The default map is 8 buckets;
giving it a second level costs a transaction for nothing. So the map must
start one-level and grow a level, which means the promote path must be
correct from the first release.

**9.4 The format is free to change — nothing is deployed.** Earlier drafts
treated this as a break to be guarded and migrated. It is neither. No store
exists outside this tree, so on-flash compatibility is not a constraint on any
decision in this proposal: the layout, the header fields and the `map_config`
struct can all be changed outright rather than extended around.

Two consequences worth taking: **D3a can replace `initial_capacity` instead of
coexisting with it** (§7.2), and the depth field needs no compatibility story.
The `version` byte §4.3 reserves stays useful for evolution *after* something
ships — it is simply not doing any work here.

D3e still means a one-level map is unchanged in practice, which keeps the diff
small. That is now a convenience rather than a compatibility requirement.

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

**9.6 If the distribution degrades, v1 has no cure — only preventions.** Worth
stating as a cost rather than leaving implied, because the failure is silent
until it is fatal.

*What degradation is.* Keys cluster into a subset of buckets. Nothing breaks:
every key is still found, `VERIFY PASS` still passes, and the damage shows up
only as buckets growing faster than they should — more bytes per `get`, more
rewritten per `set` (K4) — until one bucket reaches the payload ceiling and
returns `-ENOSPC` (**K2**), possibly hours into a fill. Note it need not be the
*keys* that cluster: a skewed value-size distribution produces a hot bucket with
perfectly spread keys, which is what the compound-Poisson estimate got wrong at
person 9 232 (`DESIGN.md` §6.1).

*Three preventions, and they are all v1 has:*

1. **Independent level indices** (D3d). Disjoint bit ranges of a 64-bit hash, or
   separate salts. Free, and the whole fix for design-caused clustering.
2. **Offline verification.** `tools/sizing.py` through the real hash — but only
   where the population is derivable. §6.1 already records the limit: this
   application can enumerate only because its data is a pure function of an
   index (F6); one whose data arrives from outside cannot.
3. **Margin.** The strongest of the three, and the second reason §5.1 matters:
   clustering that bursts a bucket at load 4 is harmless at load 0.5. Skew
   cannot be fixed, but two levels make the map cheap enough to keep sparse that
   skew need never reach the ceiling.

*And nothing detects it,* because D3b excluded occupancy from `stat()` — rightly,
since maintaining it costs a write per insert. So the only signal is the
`-ENOSPC` that arrives too late. **§9.7 is the free exception.**

*There is no repair.* The bucket count is fixed at `create` (**K3**), so the map
cannot be re-shaped. Copying into a larger one means reading every key, and
**K6 is "no iteration"** — the data cannot be enumerated out of the store it is
stuck in. Unless the application can regenerate its dataset from an external
source, passing the ceiling means reformat and data loss. In v1 a distribution
mistake is not merely unrecoverable, it is **unmigratable**.

That is the cleanest argument for v2 that this proposal contains: splitting *is*
the in-place cure, because it re-shapes the map without needing to enumerate it.

**9.7 One free warning, and it should be in v1.** Every `set` already reads its
target bucket, so `kvhash` knows that bucket's byte size at **zero** extra cost —
no counter, no second read, no write. It can therefore say so when a bucket
crosses a threshold of the payload ceiling (60 % is a reasonable start),
by log or by return value.

This satisfies D3b's rule exactly: observed in passing, never stored. And it is
the mitigation for §9.6 — it converts K2 from a surprise that arrives when
recovery is impossible into a signal that arrives while margin still exists.
Given that v1 deliberately has no occupancy query and no repair path, a warning
that costs nothing is the difference between a limitation and a trap.

## 10. Recommendation

**Option B, implemented so that C stays reachable**, and A folded in as the
leaf-level id scheme if `blob_db` gains the range call.

**Ship v1 only** (D4, decided): a two-level map with eagerly created sub-maps,
no splitting — and, per D3e, **reached only by declaring a population that needs
it**. A caller who says nothing gets today's small one-level map, unchanged on
flash and in code. It carries no crash-consistency work beyond what the
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

### 11.1 Test coverage — not a prerequisite, with one exception

`tests/lib/containers/` is a skeleton: a README saying "planned but not yet
implemented", no `testcase.yaml`, so `west twister` skips it. An earlier draft
of this section called that a blocking prerequisite. **It is not**, and the
reason is worth stating rather than assumed:

- **Nothing is deployed.** No store exists in the field, so on-flash
  compatibility is not a constraint and there is no migration to protect. §9.4
  shrinks to nothing on the same grounds.
- **Coverage exists, indirectly and end to end.** `tests/lib/kvdb` exercises
  `kvhash` through L3. More usefully, `app_cbor_persondb` is a correctness
  harness in its own right: a fill of 8 000 records followed by `VERIFY PASS`
  compares sampled persons field by field against the generator and resolves
  every one of their cards back through the credential index, then does it
  again after a mutation round. A two-level map that put a key somewhere it
  could not find again would fail that on the first run.

So a dedicated suite is worth having and is not worth blocking on. Write it when
convenient; it is not what stands between this proposal and a first commit.

**The exception is distribution, and it does not need a test suite.** A
correlated hash (§11.2) is the one defect this coverage would *not* catch: every
key is still found, so `VERIFY PASS` passes, and the damage shows up only as
clustering — a bucket bursting early (K2), or a lookup cost quietly worse than
the tables in §5. Silent degradation is exactly the failure mode this proposal
exists to remove, so it needs a check.

The cheap one already exists: **`tools/sizing.py` enumerates the real population
through the real hash**, which is what it was built for. Extending it to model
the two-level split — top index and sub index, reporting the fullest bucket
across both — costs a few lines and answers the question offline, before any
firmware runs. That is the check to write, in preference to a Zephyr test suite.

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

### 11.3 The decision that gates code

**D3a (config shape)** decides the interface and, with it, whether v1 needs a
promote path at all: if the container is told the expected population at
`create`, depth is a create-time choice and there is no one-level → two-level
transition to build. §7 argues this is choosing between two contradictory
existing specifications rather than inventing an API.

**D1 is decided** (contract revision, §7.2), and with nothing deployed it no
longer carries a compatibility question either. What remains under it is
mechanical: five call sites, one provider.

So D3a is the last thing between this proposal and a first commit — and §7.2
argues it is mostly pre-empted, since a revision lets the declarative fields
replace the field rather than coexist with it.

### 11.4 What D4 already cleared

For the record, so these are not re-opened as blockers:

| | why it is not blocking v1 |
|---|---|
| **D2** `alloc_id_range()` | v1 stores child ids in directories; Option A is not on the v1 path |
| **D3** promotion | disappears if D3a is declarative — depth is chosen at `create` |
| **D3c** per-map bucket sizing | redundant once the container derives bucket size from a declared population (§7.3) |
| p99 write measurement | moves to v2 with the splitting it was meant to characterise |
| RAM for a second directory | not needed — one `dir_buf` suffices (§9.5) |

## 12. Decisions this proposal needs from review

- **D0. DECIDED: no, the test suite does not gate this.** Nothing is deployed,
  so on-flash compatibility is not a constraint; `tests/lib/kvdb` covers
  `kvhash` indirectly; and `app_cbor_persondb`'s `VERIFY PASS` over 8 000
  records and their credentials would catch a map that lost track of a key on
  the first run (§11.1). Write `tests/lib/containers/` when convenient.

  **What still needs a check is distribution** — a correlated two-level hash
  (D3d) passes every functional test and shows up only as clustering, which is
  the silent-degradation failure this proposal exists to remove.
  `tools/sizing.py`, extended to model the two-level split, answers that
  offline. That is the residue of this decision, and it is a few lines rather
  than a suite.
- **D1. DECIDED: contract revision.** `kvhash` evolves in place; no `kvhash2`,
  no parallel Kconfig symbol, no duplicated bucket code. §7.2 is the impact
  list. (§11.3)
- **D2.** Does `blob_db` gain `alloc_id_range()`? If not, Option A and Option C
  are both off the table and B is the only route.
- **D3.** Fixed two-level, or one-level promoted on growth? §8 answers the
  shape: **uniform levels with depth in the directory header, promoted, shipped
  at two.** What review must confirm is that promotion is in v1 rather than
  deferred — a fixed two-level map is cheaper to build and forecloses the third
  level (§8) and the small-map case (§9.3).
- **D3a. DECIDED: declarative, and `initial_capacity` is removed.** Not
  deprecated, not documented-as-buckets, not kept beside the new fields —
  removed. Its two readings are both absorbed: "expected entry count" *is*
  `expected_entries`, and "bucket count" is derived (§7.1). Nothing in the
  proposed implementation reads it.

  The `bucket_count_override` an earlier draft proposed is struck on the same
  grounds — it was the same field renamed, and it would have re-opened exactly
  what D5's first criterion closes.

  Migration is two call sites, and both say something truer afterwards:

  | today | becomes |
  |---|---|
  | `app_perf_kvdb:452` — `.initial_capacity = N_KEYS / 2` (384 asked, 127 delivered) | `.expected_entries = N_KEYS` |
  | `tests/lib/kvdb:186` — `.initial_capacity = 16` | `.expected_entries = 16` |

  The first was expressing "about two entries per bucket" through a field the
  container reads as buckets, then silently receiving a third of it — K9(b)
  live in the tree. A declared population cannot be clamped away.

- **D3f.** Confirm `SMALL_MAP_LOAD` (§7.1), proposed at **4**, which puts the
  one-level/two-level boundary near a thousand entries. It exists because
  §5.2's ~1-entry-per-bucket target is a write-traffic optimisation and small
  maps have no write traffic to optimise; without it a 300-key store pays 28
  sub-map creations (B7) for nothing. A constant, not a format field.

  **With `initial_capacity` gone, the derivation rule is the entire sizing
  interface** — no caller can correct it. That raises the stakes on getting it
  right, and is the strongest argument for **D3b** (`kvhash_info()`): if the
  container decides the geometry alone, an application must at least be able to
  see what it decided.
- **D3d.** How are the two level indices derived from one key (§11.2)? Split a
  64-bit hash, or salt per level? Unspecified today, and getting it wrong
  clusters keys into exactly the failure K2 punishes — silently, since every key
  is still found (§9.6).

  **Folded in: the free bucket-fill warning (§9.7).** A `set` already reads its
  bucket, so reporting that it has crossed a threshold of the ceiling costs
  nothing — no counter, no extra read, no write. Proposed for v1 at 60 %,
  by log or return value. It belongs with this decision because it is the
  mitigation for this risk: v1 has three preventions against degradation, no
  detection once D3b excluded stored occupancy, and **no repair at all** — K3
  fixes the bucket count and K6 gives no iteration, so a degraded map cannot be
  re-shaped *or* copied out of. A warning that costs nothing is what separates
  that from a trap.

  Settle the threshold and whether it is a log line, a return code, or both.- **D3e. DECIDED: `{0}` builds one level, small — today's behaviour
  unchanged.** The default serves the small single-level map; two levels are
  opted into by declaring a population that needs one (§7.1). Nothing regresses
  for callers who say nothing, and v1's lack of promotion therefore introduces
  no new way to be wrong — the undeclared map is served exactly as it is today.
  **`ONE_LEVEL_MAX_BUCKETS` = 255**, confirmed: a 2 048 B directory and one blob
  at `create`. Set by create cost (B7, ~1.1 s per fresh bucket on the DK), not
  by lookup bytes, whose crossover is near 25 entries. A tunable constant, not a
  format field, so it can be revisited with measurements without invalidating
  any store.

  The rest of §7.1's rule is not offered as a choice, because it is what makes
  the field meanings enforceable: `NULL` ≡ `{0}` ≡ no information; each field
  independently unset at zero; and **a field that is set is honoured or
  refused, never silently clamped** — which closes K9(b) as well as K9(a).
- **D3b. DECIDED: geometry readback in v1, and nothing that costs a write.**
  `map_ops.stat()` plus a `create()` out-param, reporting only what the map
  already persists to function: depth, fan-out, bucket count, entry-size ceiling
  (§7). One or two blob reads on the reopen path, zero through `create`.

  **Entry count is excluded**, because maintaining it means rewriting a
  directory on every insert and delete — reintroducing the K5 traffic this
  proposal exists to remove — and computing it on demand means a full walk
  (B4). Largest-stored-entry likewise. No new persisted fields, no counters, no
  writes: if a value cannot be derived from what the map already stores, it is
  not in the struct.

  What it buys is the one thing D3a made unobservable: with no override and no
  readback, the derivation rule would be trusted rather than checkable. What it
  does not buy is anything a running system needs — there is no functional
  caller, only the report path, a post-`create` log line, and tests.
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
