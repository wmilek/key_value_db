# Findings — limitations of the stack, as seen from an application

Status: **v0.2 — seeded from code reading during design (2026-08-09).**
No measurements yet; the app is not implemented.

This is the *probe* output of `app_cbor_persondb` (see `DESIGN.md` §1). Every
drawback the app trips over on its way to a working 4 MiB person database is
recorded here — whether or not it is worth fixing, and whether or not the app
works around it. Per `DESIGN.md` D2 the app does **not** work around them; that
is what keeps this register honest.

Nothing here is a bug report against a broken library. `blob_db` does what its
contract says. These are the places where the *contract itself*, or its v1
implementation, costs an application something — the input to deciding what v2
should change.

**Status:** `read` = derived from the code, not yet exercised · `hit` = the app
ran into it · `measured` = quantified in `RESULTS.md` · `wontfix` = understood
and accepted.

**Severity** is from the application's point of view: `major` shapes the design
or the numbers, `moderate` costs effort or correctness margin, `minor` is
friction.

---

## L1 — `blob_db`

### B1 — Every operation reads a whole sector (major, `read`)

`blob_db_get`, `update`, `delete` and `exists` all call `read_bucket()`, which
issues `blob_db_store_read(bucket_offset(bid), buf, st.peb_size)` — the entire
erase block — and then walks it in RAM.

> `blob_db.c:870`, `:928`, `:990`, `:1052`; `read_bucket()` at `:556`

On the DK's MX25R6435F that is **64 KB of SPI traffic per operation**, whether
the payload is 5 bytes or 4 KB. It is the single cause of every latency figure
here: the measured 16.9 ms `blob_db` read (`app_perf/RESULTS.md`) is 64 KB at
~4 MB/s, not a seek or an erase.

Impact: resolving a credential and checking a permission costs 2 map gets =
**4 sector reads = 256 KB** to answer a question about ~365 B of data — read
amplification of roughly **700×**.

Direction (not implemented): the format already stores per-slot lengths, so a
get could read the bucket header, walk slot headers with short reads, and fetch
only the matching payload. On NOR random reads are cheap; the whole-sector read
buys simplicity and costs the entire performance envelope.

### B2 — Compaction costs five sector erases (major, `read`)

`compact_commit()` sequences: master ← COMPACTING, erase+write scratch,
erase+write bucket, erase scratch, master ← CLEAN. Each master write erases its
sector first. **Five 64 KB erases** to compact one bucket, at ~1.1 s each on the
DK.

> `compact_commit()`; erase cost from `app_perf_kvdb/RESULTS.md` (`prepare`:
> 1.097 s per bucket)

Impact: the ~37 MB appended during the fill drives ~1 250 compactions — about
2 h of the ≈ 2.5 h one-time population.

Direction: two of the five are the double-buffered master; one is a post-hoc
scratch erase that could be deferred into the next compaction's
erase-before-write.

### B3 — No occupancy or geometry introspection (major, `read`)

The public API exposes no partition size, sector size, bucket count, live bytes,
free bytes or fill level. An application cannot size a dataset to a fraction of
the medium (**this is R-E**), predict `-ENOSPC`, report how full it is, or
decide how many buckets to hand `blob_db_prepare()` (B7).

Impact: to report "50 % of the external flash" the app must read
`FIXED_PARTITION_SIZE(storage_partition)` from devicetree, going behind the
library's back and duplicating knowledge `blob_db` already holds in `struct st`.
Physical occupancy — live bytes plus not-yet-compacted garbage — stays
unobservable, so achieved fill can only be reported as *logical* bytes.

Direction: a `blob_db_stat()` returning geometry plus live/free/garbage counts.
`mount()` already computes most of it.

### B4 — `count()` / `iterate()` are O(n²) (moderate, `read`)

Both are documented O(n²) in entry count, because deciding whether a slot is
superseded rescans its bucket. At the ~4 300 blobs this app creates they are a
diagnostic costing seconds, not a routine health check.

> `blob_db.h` @ref blob_db_count

### B5 — One Kconfig symbol does five unrelated jobs (major, `read`)

`config BLOB_DB_MAX_PAYLOAD_LEN … range 1 4096`. Nothing in the on-flash format
requires 4096 — a slot is `14 + val_len` inside a sector that is 64 KB on the
DK. But raising it is not the fix it looks like, because the symbol is
overloaded:

| # | Job | Site |
|---|---|---|
| 1 | L1 write limit — `update` rejects a longer payload | `blob_db.c:915` |
| 2 | L1 walk sanity bound — the check that stops a slot walk running off into garbage | `blob_db.c:201`, `:502` |
| 3 | L1 stack buffer — `append_slot` builds the slot in `14 + MAX + 32` bytes **on the stack** | `blob_db.c:593` |
| 4 | L2 bucket size **and** bucket count — `MAX_BUCKETS = (MAX−8)/8`, plus `2 × MAX` of static `.bss` | `kvhash.c:47,54-55` |
| 5 | L1½ registry capacity — `BUILD_ASSERT(8 + 16 × ROOTREG_MAX_ROOTS ≤ MAX)` | `rootreg.c:47` |

Job 4 is two jobs wearing one hat: **bucket size and bucket count are the same
number**, so capacity is `MAX²/8` and neither factor can be moved alone. That
coupling is the actual cause of K1, and it is why more payload is not more
capacity in any useful sense.

**What a 10× raise (4096 → 40960) would do.**

Helps:

- **K1 dissolves.** Map capacity `MAX²/8` = 209 MB — one map holds anything an
  8 MiB part can store, so this app would not shard at all.
- **K2 softens.** A 40 KB bucket holds ~113 person entries instead of ~11, so
  the tail stops mattering and the 5.5 σ over-provisioning goes away.

Costs, in increasing order of severity:

- **RAM (B6).** `kvhash`'s two static buffers go 8 KB → **80 KB**; with
  `blob_db`'s 128 KB that is 208 KB of the nRF5340's 512 KB.
- **Stack.** `append_slot`'s frame goes ~4.1 KB → **~41 KB**. The existing
  `CONFIG_MAIN_STACK_SIZE=32768` no longer holds a single call.
- **K5 explodes if the extra buckets are used.** `MAX_BUCKETS` becomes 5 119, so
  a directory is **40 KB**, rewritten on every bucket creation (K5). ~4 400
  creations × 40 KB ≈ **176 MB**, against 15 MB today — and since a 40 KB blob
  leaves no room to append a second version in a 64 KB sector, *every one of
  those rewrites forces a compaction*: ~7 h of erases for bookkeeping alone.
- **The append log stops working above ~½ sector.** A `blob_db` bucket is one
  sector and updates are appends. A payload of `P` allows `⌊(peb−16)/(P+14)⌋`
  versions before compaction: 4 KB gives ~16, 40 KB gives **1**. The practical
  ceiling is `peb_size / versions_wanted` — for a 64 KB sector and ~16 appends,
  **~4 KB. The present cap is well matched to the DK's flash**, which looks less
  like an arbitrary limit than it first appears.
- **Portability.** A slot must fit a sector (`14 + len ≤ peb_size − 16`), so
  anything above ~4066 is unusable on a 4 KB-sector part — internal flash, and
  `native_sim`'s default. Raising the `range` is harmless; raising the *default*
  would break those targets. And nothing checks it (B9).

**The variant that does work, and what it reveals.** Raise `MAX` *and* keep
asking for ~511 buckets: the directory stays 4 KB and one map holds 20.9 MB.
But then 10 000 entries land 19.6 per bucket, so every `set` rewrites a 7.1 KB
bucket instead of 885 B — K4 worsens 8×, and the fill's appended bytes go from
~21 MB to ~39 MB.

Writing the cost out for one map of `B` buckets, `N` entries of size `e`:

```
bucket rewrites  ≈ e·N·(N/B + 1)/2        falls with more buckets
directory rewrites ≈ 8·B²                 rises with more buckets
```

| Layout | bucket rewrites | directory rewrites | total |
|---|---|---|---|
| 1 map, 511 buckets | 37.4 MB | 2 MB | 39 MB |
| 1 map, 1 046 buckets (the optimum for a flat directory) | 17.3 MB | 8.7 MB | 26 MB |
| **8 maps × 511 (this app)** | **6.2 MB** | **15 MB** | **21 MB** |

Sharding wins because *N maps × 511 buckets is a two-level directory*: 4 088
buckets addressed through eight 4 KB directories instead of one 32 KB one. The
workaround K1 forces on the application is, by accident, the better structure.

**Direction.** Raising the number is the wrong lever. In value order:

1. **Let the `kvhash` directory span more than one blob** (or derive bucket ids
   from the root id instead of storing them). This removes `MAX_BUCKETS`, hence
   K1's 2.09 MB ceiling, hence K5's 4 KB rewrite — and leaves `MAX_PAYLOAD` free
   to stay at the ~4 KB the append log wants. It is the one change that fixes
   K1, K3, K4 and K5 together, and it is what the two-level table above is
   pointing at.
2. **Split the symbol** so L1's write limit, L2's bucket size and L2's bucket
   count are three Kconfig options, not one.
3. Raise the `range` maximum (it is arbitrary) but not the default, and add the
   mount-time check of B9.

**Eliminating it entirely is worse.** Job 2 needs *some* bound or a corrupt
`val_len` walks a bucket off its end; job 3 needs a bounded buffer or a
streaming write path; job 4 needs a size for its static buffers. "No limit"
really means "derive it from `peb_size` at runtime", which sizes those buffers
from `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` — 64 KB — making `kvhash` cost 128 KB of
`.bss` on top of `blob_db`'s 128 KB. That is B6 doubled, to buy a payload size
the append log cannot use anyway.

### B9 — Nothing checks that a maximum-size payload fits a sector (moderate, `read`)

A slot needs `14 + val_len` bytes inside a sector whose usable space is
`peb_size − 16`. `mount()` validates `peb_size ≤ CONFIG_BLOB_DB_SECTOR_BUF_SIZE`
and nothing else; there is no assertion, at build or at mount, that
`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 14 ≤ peb_size − 16`.

Configure `MAX_PAYLOAD_LEN=4096` against a 4 KB-sector part — the Kconfig
permits it, and it is `native_sim`'s default geometry — and a maximum-size
payload can never be stored. The failure is not a refused mount but `-ENOSPC`
from `blob_db_update` (`blob_db.c:963`) at whatever moment an application first
writes a large enough blob, after compacting a bucket for nothing.

> `blob_db_mount()` sector check at `blob_db.c:272`; the `-ENOSPC` at `:963`

Direction: check it at mount, where `peb_size` is known, and fail with
`-ENOTSUP` like the sector-size check immediately above it.

### B6 — 128 KB of `.bss` for sector buffers (moderate, `read`)

`g_bbuf` and `g_bbuf_new` are each `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` — 64 KB on
the DK, so **128 KB, a quarter of the nRF5340's 512 KB RAM**, permanently
resident. `kvhash` adds 2 × `MAX_PAYLOAD` = 8 KB (K7).

Awkward beside principle P3 ("minimum RAM usage … no caches"). It *is* O(1), but
the constant is set by the flash part's erase-block size — the one thing the
application cannot change.

Direction: B1's short-read get would remove the need for one of the two.

### B7 — First write to a fresh bucket stalls ~1.1 s (moderate, `read`)

An `update` landing in a never-written bucket erases and headers that sector
in-line. `blob_db_prepare()` moves the cost off the hot path, but the
application must drive it — and cannot know how many buckets to prepare, because
bucket count is not exposed (B3). `app_perf_kvdb` passes `N_KEYS * 2` as a guess.

### B8 — Crash during multi-blob creation leaks blobs permanently (major, `read`)

Building a structure out of several blobs and publishing it with one final
atomic write is the prescribed pattern — `kvdb` uses it (`kvdb.c:118`: "a crash
before this commit leaves `struct_root` orphaned"), and `access_open` uses it for
nine map roots plus a superblock (`DESIGN.md` §9).

The comment says such blobs are "reclaimed by a later format". That is the
whole story: **there is no reachability GC**. Compaction reclaims tombstones and
superseded slots only — a blob that is live but unreferenced is invisible to it
and survives every compaction, forever.

Principle P7 states "**No permanent leak (must)** … Long-term accumulation of
leaked space is not accepted." A device that loses power during enrollment,
repeatedly, over years, accumulates exactly that.

Direction: either a mark-and-sweep from the registry (needs iteration, K6/B4),
or a durable creation-intent list that a later mount can roll back —
`BLOB_CONTAINERS_INTENT` is scaffolded for something like this but not
implemented.

---

## L2 — `kvhash` and the Map shape

### K1 — A map tops out near 2.09 MB (major, `read`)

The directory must fit one blob payload, so `n_buckets ≤ (MAX_PAYLOAD − 8)/8 =
511`, and each bucket's packed list must also fit one payload. Capacity is
`MAX_PAYLOAD × (MAX_PAYLOAD − 8)/8 ≈ 2.09 MB`.

> `kvhash.c:45-50`

Impact: 10 000 person records (~3.6 MB) **cannot live in one map**. The app
shards across eight, and implements its own fan-out. Nothing in L2 or L3 helps:
naming, hashing, fan-out and per-shard capacity accounting are all the
application's problem (V4).

### K2 — Overflow is per-bucket and depends on entry-size variance (major, `read`)

`kvhash_set` returns `-ENOSPC` when one bucket's packed list would exceed a
payload — while the store as a whole may be nearly empty. No split, no rehash,
no overflow chain. The safe load factor is therefore not a property of the
container but a statistical property of *the application's key hash and
value-size distribution*, which the application must work out itself.

Concretely: the same 511-bucket map holds 25 000 credential entries (23 B)
comfortably but only a few thousand person entries (362 B).

**The evidence is the over-provisioning, not a failure.** Per `DESIGN.md` D11
the app stays inside the limit rather than working around it — no overflow
chains, no retry-on-`-ENOSPC`. But because there is no per-bucket occupancy
query (K10) and no growth path (K3), the margin has to be chosen **blind and up
front**, and getting it wrong surfaces as `-ENOSPC` hours into a fill. That
forces a 5.5 σ margin: **eight people maps where four would have held the
bytes**, mean bucket load 22 % of capacity. Half the provisioned capacity buys
nothing but ignorance of the tail. The compound-Poisson table is in
`DESIGN.md` §6.1; that an application has to compute one at all is the finding.

Direction: bucket splitting, an overflow chain, or — cheapest and most useful —
simply reporting per-bucket occupancy so an application can see the cliff coming
and size to 3 σ instead of 5.5 σ.

### K3 — `n_buckets` is fixed at create time (major, `read`)

Chosen from `map_config.initial_capacity` on first create, never changed. A map
that outgrows its bucket count has no growth path short of rebuild — and rebuild
needs iteration, which does not exist (K6). Capacity must be planned exactly,
before the first insert, at a layer with no visibility into how much flash
exists (B3).

### K4 — Every `set` rewrites the whole bucket blob (major, `read`)

Insert, update and delete read the bucket, edit it in RAM, and
`blob_db_update()` the entire packed list back. Write amplification is
`bucket_bytes / entry_bytes` — about 8–16× at the fill levels here — paid on
every write, including a one-byte change.

### K5 — The directory is rewritten on every first insert into a bucket (major, `read`)

A bucket blob is created lazily; publishing it writes the whole directory back.
The directory is `8 + 8 × n_buckets` = **4 KB at 511 buckets**.

> `kvhash.c:319-328`

Impact: filling eight people shards creates ~3 700 buckets, so ~3 700 directory
rewrites of 4 KB ≈ **15 MB of flash writes for bookkeeping alone** — comparable
to the ~21 MB the data costs, and a large share of B2's compaction load.

Direction: the directory is a dense array of ids; a bucket's id could be derived
rather than stored, or the directory chunked so a publish rewrites 64 B.

### K6 — No iteration (major, `read`)

`map_ops` has no `iterate`. An application cannot enumerate, back up, migrate,
fsck or garbage-collect its own data. Every key must be regenerable from outside
the store — which this app arranges (F6) by making the dataset a pure function
of an index, but a deployment with externally-sourced keys cannot.

`doc/layers/l3_interfaces.md` §3 records this as deferred until `kvtree`.

### K7 — Scratch buffers are file-scope and shared across all maps (moderate, `read`)

`dir_buf` and `bkt_buf` (2 × `MAX_PAYLOAD` = 8 KB) are static and shared by
every `kvhash` map in the image. Two *different* map roots are therefore not
independent — a stronger coupling than C7's "the caller serializes", and one a
caller holding nine separate roots would not expect from the header.

> `kvhash.c:54-55`

### K8 — No way to ask whether a root holds a valid map (moderate, `read`)

`rootreg_get_or_create` may hand back an allocated-but-unbound id, and it is the
caller's job to notice and call `create`. The shape offers no
`exists`/`is_valid`, so every L2 client reimplements the same
`blob_db_get(root) == -ENOENT` probe — reaching *past* L2 into L1 to do it,
which breaks the layering the shape is supposed to provide.

### K9 — Requested capacity is reinterpreted, then silently clamped, and cannot be read back (moderate alone; **major** with K2+K3, `read`)

Three separate problems stack on one field.

**(a) The units do not match the contract.** `shape_map.h` documents
`initial_capacity` as *"expected entry count"* and `kvdb.h` repeats it as
*"entry-count hint"*. `kvhash` uses the number **verbatim as the bucket count**:

```c
static uint16_t buckets_for(size_t initial_capacity)
{
        size_t n = initial_capacity ? initial_capacity : DEFAULT_BUCKETS;
        if (n < 2)            { n = 2; }
        if (n > MAX_BUCKETS)  { n = MAX_BUCKETS; }
        return (uint16_t)n;
}
```

> `kvhash.c:97-109`, `shape_map.h:40`, `kvdb.h:74`

Entries and buckets differ by the load factor — the one number that decides
whether K2's 4 KB bucket ceiling is ever reached. Saying "I expect 10 000
entries" is read as "give me 10 000 buckets".

**(b) The clamp is silent.** `MAX_BUCKETS = (MAX_PAYLOAD − 8)/8` = 511 at
`MAX_PAYLOAD` 4096. Anything larger is clamped with no error, no warning, and
`create()` returning 0. The only trace is a `LOG_DBG` inside the container's own
log module, off in any normal build.

This is not hypothetical — **it already happens in this tree**.
`app_perf_kvdb` runs with `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=1024`, so
`MAX_BUCKETS` is 127, and asks for:

```c
.initial_capacity = N_KEYS / 2,        /* 768 / 2 = 384 */
```

> `app_perf_kvdb/src/main.c:452`, `app_perf_kvdb/prj.conf:10`

384 requested, **127 delivered**, nothing said. It happens to be harmless there
— 768 small entries over 127 buckets is 194 B per bucket — but the app's stated
intent was silently overruled by a factor of three.

**(c) It cannot be read back.** `map_ops` has no query (K10), and the value
lives at `dir_buf[4]` inside `kvhash`'s private on-flash directory format.
Reading it means `blob_db_get(root)` plus parsing L2's internals from the
application — reaching *past* L2 into L1 **and** depending on a layout the
container does not publish. Nor can an application compute the ceiling for
itself: `MAX_BUCKETS`, `DIR_HDR_LEN` and the 8-bytes-per-id are all private to
`kvhash.c`.

**Why the combination is worse than the parts.** The bucket count is fixed at
create time and persisted (K3), so a wrong value is baked into the store
forever; the store gives no occupancy signal (K10); and the failure surfaces as
a `-ENOSPC` from one bucket while the medium is nearly empty (K2), potentially
hours into a fill, with no repair path short of a rebuild that needs iteration
(K6) that does not exist.

**The sharpest edge is cross-Kconfig coupling.**
`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is a single global symbol, and `MAX_BUCKETS`
is derived from it. Another subsystem in the same image lowering it to the
Kconfig default of 256 takes `MAX_BUCKETS` from 511 to 31 — map capacity from
2.09 MB to **7.9 KB, a 265× drop** — while this application still asks for 511,
still gets `0` back from `create()`, and dies a few hundred records into the
fill. A build-time change made elsewhere silently invalidates a persisted
capacity plan, and there is no runtime check that can catch it.

**A related trap in the same function:** `cfg == NULL` or
`initial_capacity == 0` yields `DEFAULT_BUCKETS = 8` — an eight-bucket, ~32 KB
map that overflows at roughly ninety person-sized entries. For `kvdb` that is
reached by `kvdb_open(db, name, NULL)`, which the header presents as the
ordinary way to accept defaults.

**What this app can do about it:** only a `BUILD_ASSERT` on
`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`, replicating kvhash's private formula in
application source. That defence is worth having, and having to write it is
itself part of the finding.

Direction, cheapest first: return `-ENOSPC` (or at minimum `LOG_WRN`) instead of
clamping; export the capacity formula from `kvhash.h`; add the `map_ops.stat()`
of K10 so the delivered count is readable; and settle whether the field means
entries or buckets, in the shape's own words.

### K10 — No entry count or size on a map (moderate, `read`)

`map_ops` has `create`/`get`/`set`/`del` and nothing else. An application cannot
ask how many entries a map holds or how close a bucket is to K2's cliff, so it
cannot decide when to add a map. With K6 it cannot even count them by walking.

This is the finding that makes K2 expensive rather than merely present: a
`map_ops.stat(root)` returning entry count and peak bucket occupancy would let
an application size to a measured tail instead of a guessed one, and would turn
K2 from a cliff into a gauge.

---

## L1½ — `rootreg`

### R1 — `ROOTREG_MAX_ROOTS` defaults to 8 (minor, `read`)

Eight registered roots is below what a sharded dataset needs if each shard is a
named `kvdb` instance — the nine-instance L3 variant in `DESIGN.md` §9 does not
fit the default. The app's L2 layout uses one entry, so it never trips this, but
it is a sharp edge for the L3 route: the failure is `-ENOSPC` from
`kvdb_open`, at a layer that says nothing about registry capacity.

---

## L3 — `kvdb`

The app does not link `kvdb` (`DESIGN.md` D10). These come from evaluating it as
the implementation route and rejecting it, which is itself a finding about L3's
current reach.

### V4 — Nothing composes shards, so L3 stops being useful past ~2 MB (major, `read`)

K1 makes sharding mandatory past ~2.09 MB, and `kvdb` offers no help: the
application invents instance naming, the key→shard hash, read fan-out and
per-shard capacity accounting regardless. What `kvdb` still charges for that is
nine registry entries, nine meta blobs, eighteen sector reads at boot, and a
collision with R1's default — in exchange for a naming feature a sharded app
does not need, because it has one superblock and knows its own shards.

L3's current audience is therefore **small stores of a few named instances**.
Every application larger than one map drops to L2, as this one does.

> The finding most likely to justify an L3 addition rather than an L1/L2 fix: a
> sharded-map interface over N `kvhash` maps, owning the fan-out, the capacity
> accounting (K10) and the growth path (K3).

### V1 — `kvdb_has()` reports errors as "present" (moderate, `read`)

```c
int rc = db->ops->get(db->root, key, strlen(key), NULL, 0, NULL);
return rc != -ENOENT;
```

> `kvdb.c:213-221`

`-EIO` (flash error), `-EINVAL` and `-ENOMEM` all yield `true`, while the header
states "true iff `kvdb_get` would find the key". A flash fault reads as an
existing key — the wrong direction for anything making an access decision.

Direction: return `rc == 0 || rc == -ENOMEM`, or give `kvdb_has` an `int` return.

### V2 — A value's size cannot be learned cheaply (moderate, `read`)

`get` sets `*out_len` even on `-ENOMEM`, so a size probe is possible — but costs
the same two sector reads as the real get (B1). For variable-length CBOR values
an application must size a worst-case buffer or pay double. This app sizes a
768 B stack buffer, on a stack `prj.conf` already has to enlarge for `blob_db`.
Applies equally to L2 (`map_ops.get`), so dropping a layer does not help.

### V3 — No size, capacity or fill query (moderate, `read`)

Inherits B3, K3 and K10 unchanged. Nothing at L3 adds introspection that L2
lacks.

---

## Not yet exercised

Findings that need the running app before they can be stated honestly:

- whether the §6.1 sizing rule holds — a single `-ENOSPC` during a full fill
  means it did not, and the margin needed is larger still (F11 counts them,
  A8 asserts zero);
- whether the fill matches the ≈ 2.5 h estimate, and how much of it is B2;
- compaction behaviour and latency spikes at ~50 % occupancy;
- whether CBOR encode/decode is measurable at all next to B1 (expected: no);
- whether K7's shared scratch buffers cause any surprise with nine live maps;
- `native_sim` versus DK divergence, which would be a finding about the
  `native_sim` model rather than about the stack.
