# Findings — limitations of the stack, as seen from an application

Status: **v0.6 — re-measured against `main` after the large-payload work
landed, and on the nRF5340-DK at 1 000 persons (2026-08-15).** Numbers below
are from `RESULTS.md`. N1 is closed by the board; a full-scale 10 000-person DK
run remains the only open measurement (`A4`), and until it happens the DK
figures here are a tenth-scale store — see N1 for what that costs the headline.

One finding was added by a code review of the application itself: **B12**,
where `blob_db`'s compaction lowers the durable id ceiling it is supposed to
raise. The same review found four consistency defects in *this application*
(a replace that left withdrawn credentials granting access, an assign whose
redo did not converge, an unchecked card reassignment, and silent truncation of
over-long ids). Those are fixed, and deliberately not recorded here: this
register is for limitations of the stack, and an application's own bugs do not
belong in it.

**Five findings are now closed by `main`.** B1, B9 and B10 are fixed outright,
B5 loses two of its five jobs, and B3 is half-answered by the new
`blob_db_iostats_get()`. What follows marks them `closed` rather than deleting
them: the register is the record of what an application hit, and a fixed
finding with its before/after numbers is more use than a silent gap. The
remaining open ones are listed in the summary below.

| | |
|---|---|
| **Closed by `main`** | B1, B5 (partly), B9, B10, B3 (partly) |
| **Still open** | B2, B4, B6, B7, B8, B11, B12, K1–K11, V1–V4, R1, X1 |
| **Newly observed** | N1 — transaction count rose sharply as byte count fell (closed on hardware: ~65 µs per transaction) · B12 — compaction lowers the durable id ceiling ([issue #14](https://github.com/wmilek/key_value_db/issues/14)) |

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

## Cross-reference — the large-payloads work (**now merged**)

Reviewed against this register while it was still branch
`claude/blob-db-max-payload-increase-6qobv5`; it has since landed on `main`
(PR #10). The predictions below are kept as written, next to what actually
happened, because two of them were wrong in instructive ways:

- **B6 was predicted "fixed" and is not.** The streaming walk removed the
  *reason* the read path needed a resident sector, but `g_bbuf`/`g_bbuf_new`
  remain — compaction still needs a whole image. RAM after the merge:
  157 688 B, unchanged. Predicting a RAM win from a read-path change skipped
  the write path.
- **The K11 disagreement stands.** The merged `kvhash` is untouched, so the
  directory is still read on every operation and a map get is still two
  `blob_db` calls. Nothing in the merge makes the pread-directory sketch
  cheaper than derived bucket ids.

The B1 prediction was right, and larger than claimed: 19× fewer bytes, not
"~1× amplification", but with the transaction-count caveat now recorded as N1.

| Finding | Proposal's effect |
|---|---|
| **B1** whole-sector reads | **Fixed** — cost addendum §3 mitigation 3, the streaming slot walk: "cuts read amplification to ~1×". Ranked #2, ahead of Stage 2 itself. |
| **B6** 128 KB `.bss` | **Fixed** by the same change — "`.bss` on the QSPI board from 128 KB to ~1 KB". |
| **B5** one symbol, five jobs | **Partly.** Fixes job 3 (`append_slot` staged in `.bss`, not a `MAX_PAYLOAD` stack frame) and job 1 (mount-time bound instead of a Kconfig `range`). Deliberately does **not** touch job 4 — `MAX_PAYLOAD → MAX_BUCKETS` is left alone, and the companion note says so explicitly. |
| **B9** no payload-vs-sector check | **Fixed** — Stage 1 item 3, and §1.1 sharpens it (see B10). |
| **B8** permanent orphan leak | **Partly.** Introduces exactly the machinery B8 asks for — an owner-tagged segment plus a mount-time sweep — but only for L1's own segments. Application-level orphans (this app's seventeen map roots) are still uncollectable. Its §3.2 argues the general case cannot be solved above L1, which is the same conclusion from the other side. |
| **K1** `MAX_BUCKETS` cap | **Partly**, as an explicit non-goal followed by a follow-up sketch: a pread directory lifts the cap. See the caveat below. |
| **K5** directory rewrite | **Partly**, same sketch — and only when the directory is large enough to segment. |
| **K7** shared scratch buffers | **Halved** — `dir_buf` disappears under the pread directory. |
| **K11** directory read per op | **Not fixed; arguably worsened.** See below. |
| **B2** five erases per compaction, **B3** no introspection, **B4** O(n²) walks, **B7** `prepare()` sizing, **K2** bucket overflow, **K3** fixed bucket count, **K4** bucket rewrite amplification, **K6** no iteration, **K9** silent clamp, **K10** no `stat`, **V1**–**V4** | **Untouched.** Out of scope, and the companion note is explicit that `kvhash` needs no change. |

**Where I disagree — the pread directory and K11.** The companion note's
follow-up replaces `dir_bucket(idx)` with
`blob_db_read(root, 8 + idx*8, &id, 8)` and concludes that the directory "never
needs to be resident", removing the bucket cap. The RAM claim is right. The
access-cost claim does not follow:

- an **inline** directory still costs one bucket read to locate the slot, so
  pread saves RAM and nothing else;
- a **segmented** directory costs index-read + segment-read = **two** flash
  operations where today's whole-blob get costs one (the proposal's own §6.8
  gives pread as 2 reads).

So a map get goes from 2 flash operations to 2 or 3, never to 1. The same
applies to `dir_set_bucket` → `blob_db_write`: NOR cannot program a programmed
byte back up, so an 8-byte pwrite still appends a fresh slot — the whole
directory if inline, one segment if not. K5 improves only in the segmented
case, and not by much.

The derived-bucket-id design in **K1** reaches further for less: allocate the
bucket ids contiguously at create time, compute
`bucket_id = base_id + hash % n_buckets`, and the directory becomes 16 fixed
bytes that live in the map handle. Bucket cap gone (K1), rewrite gone entirely
(K5), `dir_buf` gone (K7), **and the directory read gone (K11) — one flash
operation per map get instead of two.** It needs no large-payload support, no
new API and no format break beyond the root record, so it is independent of
this proposal and cheaper than its follow-up. Worth putting to the proposal's
author as an alternative to the pread-directory sketch.

**Sequencing.** Stage 1 is a deliberate, breaking on-flash format change. If it
lands after this app, existing stores — including a completed 10 000-person
fill — are unreadable and must be repopulated, at ≈ 2.2 h. Landing Stage 1
first is worth ~130 KB of RAM and ~2× on every read to this app, and would move
the numbers in `RESULTS.md` substantially, so **the measurements should be
taken after it, not before.**

---

## L1 — `blob_db`

### B1 — Every operation reads a whole sector — **CLOSED by `main`** (was major)

`blob_db_get`, `update`, `delete` and `exists` all call `read_bucket()`, which
issues `blob_db_store_read(bucket_offset(bid), buf, st.peb_size)` — the entire
erase block — and then walks it in RAM.

> `blob_db.c:870`, `:928`, `:990`, `:1052`; `read_bucket()` at `:556`

On the DK's MX25R6435F that is **64 KB of SPI traffic per operation**, whether
the payload is 5 bytes or 4 KB. It is the single cause of every latency figure
here: the measured 16.9 ms `blob_db` read (`app_perf/RESULTS.md`) is 64 KB at
~4 MB/s, not a seek or an erase.

**Measured, pre-merge** (every figure in this paragraph is the *before* half of
the comparison below). Resolving a credential and checking a permission is 2 map
gets = **4 sector reads = 256 KB of flash to answer a question about 365 B** —
read amplification of **711×**. A negative lookup is worse in relative terms: 128 KB
to learn that a card does not exist, **26 214×**. A write is 9.98 blob
operations and 638 KB (1 799×), because the credential index is maintained per
card. See `RESULTS.md` §4.

**On hardware, pre-merge:** an access decision measured **114.2 ms** on the DK
— 4 sector reads at 28.8 ms each. That confirmed the finding on the target and
put a real constant behind it.

**Fixed** by `7f10295 blob_db: walk buckets by slot header instead of reading
whole sectors`, which is exactly the direction this finding proposed. Re-measured
with the same benchmark on the same host:

| bench | before — flash per op | after | speedup (native_sim µs/op) |
|---|--:|--:|--:|
| `check` (R-D) | 256 KB, **656×** | **13.3 KB, 33×** | 580 → **42 µs**, 14× |
| `byid` | 128 KB, 348× | 6.5 KB, 17× | 293 → 23 µs |
| `miss` | 128 KB, 5 698× | 6.8 KB, 294× | 285 → 21 µs |
| `put` | 638 KB, 1 516× | 87 KB, 202× | 2 406 → 1 020 µs |

An access decision moves **19× less flash**. (The "before" byte figures were
modelled as `map operations × sector size`, which was accurate for code that
read whole sectors; the "after" figures are measured through
`blob_db_iostats_get()`. The µs column is measured identically on both sides.)

See **N1** for what the same change did to the *transaction* count, which is
the part still worth watching on a serial part.

### B2 — Compaction costs five sector erases (major, `read`)

`compact_commit()` sequences: master ← COMPACTING, erase+write scratch,
erase+write bucket, erase scratch, master ← CLEAN. Each master write erases its
sector first. **Five 64 KB erases** to compact one bucket, at ~1.1 s each on the
DK.

> `compact_commit()`; erase cost from `app_perf_kvdb/RESULTS.md` (`prepare`:
> 1.102 s per bucket, re-measured on main hardware)

Impact: the ~37 MB appended during the fill drives ~1 250 compactions — about
2 h of the ≈ 2.2 h one-time population.

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
- **Stack.** `append_slot`'s frame goes ~4.1 KB → **~41 KB**, which no
  plausible `CONFIG_MAIN_STACK_SIZE` holds for a single call — this app now
  runs on 12 288 B, sized by measurement (`RESULTS.md` §4c).
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

### B9 — Nothing checks that a maximum-size payload fits a sector — **CLOSED by `main`** (was moderate)

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

**Fixed**: `blob_db.c` now refuses at mount when
`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` exceeds what the geometry can sustain, and it
uses B10's *rewrite* bound rather than the weaker write-once one.

The application's own `BUILD_ASSERT` and runtime check are **gone**, and that is
the point of the fix rather than a loss. Restating the inequality app-side meant
restating `blob_db`'s slot overhead, its bucket header and the formula, and
using `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` as a proxy for a sector size the app had
no way to obtain. The library's version checks the same thing against the *real*
geometry, so it is not merely equivalent — it is correct where the app's could
pass and then fail at mount anyway. Three private constants left the application
with it (X1 rows 1–4). `persondb_open()` now forwards `-ENOTSUP`.

### B10 — A payload above half a sector can be written once but never rewritten — **CLOSED by `main`** (was major)

*Found by `doc/proposals/2026-08-09-large-payloads.md` §1.1 on branch
`claude/blob-db-max-payload-increase-6qobv5`; recorded here because it bounds
this application.*

B9 is about binding a payload once. **Rebinding is twice as strict.** A bucket
is an append-only log, so an update appends a second slot beside the live one;
if that overflows, `compact_bucket()` runs, and the compacted image still
contains the live slot. Room for both is therefore required:

```
sustainable max = ⌊(peb_size − 16)/2⌋ − 14
                = 2 026 B   on 4 KB sectors
                = 32 746 B  on 64 KB sectors
```

Above it the first `update` succeeds and every later one returns `-ENOSPC`
(`blob_db.c:963`) — a blob that can be created but not changed, which the
contract's "id stability" clause says should not exist.

**On the default 4 KB geometry, `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=4096` is
therefore doubly wrong**: 4096 exceeds even the write-once ceiling of 4066
(B9), and anything above 2026 is write-once in practice. The Kconfig `range`
permits both.

Generalized, the rule this app has to respect is per *`blob_db` bucket*, not
per blob: after compaction a sector holds all its live slots, and an update
needs room for one more copy of the blob being rewritten —

```
live_bytes(bucket) + slot_size(largest blob) ≤ peb_size − 16
```

With 4 KB blobs in a 64 KB sector that allows ~94 % occupancy, so this app's
~53 % is comfortable. But it means **the store's maximum safe fill is a
function of its largest blob**, which no layer reports (B3) — one more number
an application must derive for itself.

**Consequence for this application:** built on plain `native_sim` (4 KB
sectors) with `MAX_PAYLOAD_LEN=4096`, `kvhash` bucket blobs above 2026 B would
become unrewritable and the fill would fail confusingly. The app's `native_sim`
overlay sets a 64 KB erase-block size to mirror the DK, which avoids it — and
that was a silent dependency until `main` made `blob_db_mount()` check the
inequality itself against the real geometry. The app carried a `BUILD_ASSERT`
and a mount-time check of its own; both are now deleted, because the layer that
owns the constants answers the question (see B9).

### B6 — 128 KB of `.bss` for sector buffers (major, **`measured`** — still open)

`g_bbuf` and `g_bbuf_new` are each `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` — 64 KB on
the DK, so **128 KB, a quarter of the nRF5340's 512 KB RAM**, permanently
resident. `kvhash` adds 2 × `MAX_PAYLOAD` = 8 KB (K7).

Awkward beside principle P3 ("minimum RAM usage … no caches"). It *is* O(1), but
the constant is set by the flash part's erase-block size — the one thing the
application cannot change.

**Measured on the DK** (`RESULTS.md` §4a). The image uses 157 584 B of RAM:

| | bytes | share |
|---|--:|--:|
| `blob_db` `g_bbuf` + `g_bbuf_new` | **131 072** | **83.2 %** |
| main stack | 12 288 | 7.8 % |
| `kvhash` `dir_buf` + `bkt_buf` | 8 192 | 5.2 % |
| **the whole application** (`g_db`) | **248** | **0.16 %** |

The application owns 248 bytes; the two sector buffers own five hundred times
that, and 83 % of everything the image uses. Against it, the entire storage
stack costs 5.4 KB of ROM — so this stack is not large, it is *RAM-shaped*, and
shaped by a constant nobody using it can influence.

The stack tells the same story from the other side. The deepest chain in this
app is 7 640 B, and `append_slot`'s `MAX_PAYLOAD + 46` byte frame is 4 200 of
them — **55 % of the application's stack budget is one library function's local
buffer** (B5, job 3).

**Half of the direction landed, and the RAM did not move.** `main` staged the
slot buffer off the stack (closing B5's job 3, which is why this app's stack
dropped to 12 KB), and it added slot-header walking — but `g_bbuf` and
`g_bbuf_new` are still two whole sectors of `.bss`, still allocated from
`CONFIG_BLOB_DB_SECTOR_BUF_SIZE`, and compaction still needs a full image.
Re-measured after the merge: **RAM 157 688 B, essentially unchanged**, still
83 % sector buffers, against an application that owns 248 bytes.

Reading by slot header removed the *reason* the read path needed a resident
sector; retiring the buffers themselves needs the compaction path to stream
too. That is the remaining work, and it is still worth more RAM than everything
else in this register combined.

### B7 — First write to a fresh bucket stalls ~1.1 s (moderate, `read`)

An `update` landing in a never-written bucket erases and headers that sector
in-line. `blob_db_prepare()` moves the cost off the hot path, but the
application must drive it — and cannot know how many buckets to prepare, because
bucket count is not exposed (B3). `app_perf_kvdb` passes `N_KEYS * 2` as a guess.

### B8 — Crash during multi-blob creation leaks blobs permanently (major, `read`)

Building a structure out of several blobs and publishing it with one final
atomic write is the prescribed pattern — `kvdb` uses it (`kvdb.c:118`: "a crash
before this commit leaves `struct_root` orphaned"), and this app's
`create_store()` uses it for seventeen map roots plus a superblock
(`DESIGN.md` §12).

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

### K1 — `MAX_BUCKETS` caps a map near 2.09 MB nominal, ~450 KB usable (major, `read`)

**Where the number comes from.** The directory is one blob, laid out as

```
[u32 magic 'KVHA'] [u16 n_buckets] [u16 version] [u64 bucket_id] × n
```

so `dir_len(n) = 8 + 8n` and the bound is
`MAX_BUCKETS = (MAX_PAYLOAD − 8)/8`. At `MAX_PAYLOAD` 4096 that is **511**, and
`dir_len(511) = 4096` exactly — 511 is simply the largest `n` whose directory
still fits one payload.

> `kvhash.c:12-14` (layout), `:47` (bound), `:111-114` (`dir_len`)

Note what is *not* the limit: `n_buckets` is stored as a **u16**, so the
on-flash format already allows 65 535 buckets. Only the "directory is one blob"
rule binds. Lifting it needs no format break.

**Capacity is quadratic in the payload size**, because the same symbol sets both
factors (B5):

| `MAX_PAYLOAD` | `MAX_BUCKETS` | directory | nominal map capacity |
|---|---|---|---|
| 256 — **the Kconfig default** | 31 | 256 B | **7.9 KB** |
| 1024 — what `app_perf_kvdb` uses | 127 | 1016 B | 130 KB |
| 2048 | 255 | 2040 B | 522 KB |
| 4096 — the Kconfig maximum | 511 | 4088 B | 2.09 MB |

The out-of-the-box configuration gives a map holding **7.9 KB** — and with
`DEFAULT_BUCKETS = 8` (K9's NULL-config path) a map holding **2 KB**.

**Nominal capacity is not usable capacity.** 2.09 MB assumes every bucket packs
perfectly full. Real load is compound Poisson, and a bucket overflows at 4096 B
(K2), so the safe entry count is set by the *tail*. Solving
`λe + 5.5·√(λ(e² + s²)) ≤ 4096` for a 511-bucket map:

| entry size | safe entries/map | usable bytes | of nominal |
|---|---|---|---|
| 16 B | ~92 900 | 1.49 MB | 71 % |
| 64 B | ~16 700 | 1.07 MB | 51 % |
| **23 B** — this app's credentials | **~60 400** | **1.39 MB** | **66 %** |
| 256 B | ~2 260 | 578 KB | 28 % |
| **362 B** — this app's persons | **~1 280** | **463 KB** | **22 %** |
| 1024 B | ~216 | 221 KB | 11 % |
| 2048 B | ~60 | 123 KB | 6 % |

So a `kvhash` map holds **~1 280 person records, not the ~5 700 the nominal
figure suggests** — the tail costs 4×, and the loss grows with entry size
because `MAX_BUCKETS` cannot rise to compensate (K3: it cannot rise at all).
This is the table an application actually needs, and no layer provides it.

**Impact here.** 10 000 person records cannot live in one map, or in two, or in
four. The app shards across sixteen and writes its own fan-out; naming, hashing
and per-shard capacity accounting are all its problem (V4).

**Direction — stop storing bucket ids.** The directory exists only because each
bucket's id is whatever `blob_db_alloc_id()` returned when that bucket was first
touched. If instead a map allocated its bucket ids **contiguously at create
time** and derived them,

```c
bucket_id = base_id + (fnv1a(key) % n_buckets);
```

then the whole directory collapses to `{magic, version, n_buckets, base_id}` —
**16 bytes, fixed** — and:

- `MAX_BUCKETS` disappears; bucket count is bounded only by the u16 field
  (K1, K3);
- there is no directory to rewrite when a bucket is created (**K5**, ~15 MB of
  writes in this app's fill);
- lazy creation still works for free — an allocated-but-unbound id already reads
  back as `-ENOENT`, which is exactly "empty bucket", so the presence flag the
  directory was carrying is redundant;
- every operation loses a flash read (**K11**).

`alloc_id()` is a RAM operation whose only cost is advancing the durable ceiling
every 256 ids, so reserving 511 up front is two master writes. The contract
explicitly supports it: *"an allocated-but-unbound id is durable — a later mount
never re-issues it."*

A cheaper half-measure, for the record: the directory spends **8 bytes per
bucket** on a u64 id in a store whose ids start at 2 and increase by one. Four
bytes would double `MAX_BUCKETS` to 1022 and capacity to 4.2 MB — but it would
cap the store at 2³² lifetime allocations, and the "no reuse" contract is u64 on
purpose. Not recommended; noted because it shows how much of the 4 KB directory
is unused range.

### K2 — Overflow is per-bucket and depends on entry-size variance (major, `read`)

`kvhash_set` returns `-ENOSPC` when one bucket's packed list would exceed a
payload — while the store as a whole may be nearly empty. No split, no rehash,
no overflow chain. The safe load factor is therefore not a property of the
container but a statistical property of *the application's key hash and
value-size distribution*, which the application must work out itself.

Concretely: the same 511-bucket map holds 25 000 credential entries (23 B)
comfortably but only a few thousand person entries (362 B).

**It fired, and how it fired is the finding.** The design sized the store with
an analytic compound-Poisson model that put eight person maps at 5.5 σ of
headroom and ~0.03 expected overflows. The first full fill returned `-ENOSPC` at
person 9 232, with one bucket at 4 158 B against a 4 096 B ceiling.

The model was not badly built. It was fed a mean entry size that later grew by
14 %, and a right-skewed compound-Poisson tail is heavier than the Gaussian
intuition behind "5.5 σ". Both are ordinary mistakes — and **neither is
detectable at run time**, because there is no per-bucket occupancy query (K10)
and the bucket count cannot change after create (K3). The failure arrives 92 %
of the way through a fill that is a projected 2.2 h on hardware, and the only
repair is a reformat.

Sizing therefore had to move to **enumeration**: `tools/sizing.py` replays the
real hash over the real population and reports the fullest bucket
(`RESULTS.md` §9). It says sixteen maps, fullest bucket 66 % full — **sixteen
where four would hold the bytes.** That over-provisioning, and the offline
script needed to justify it, is what the missing introspection costs.

The application could only do this because its dataset is a pure function of an
index. **A deployment whose records arrive from outside cannot enumerate them
ahead of time, and has no recourse but to over-provision blindly.**

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

Impact: filling sixteen person maps creates ~5 800 buckets, so ~5 800 directory
rewrites of 4 KB ≈ **23 MB of flash writes for bookkeeping alone** — more than
the ~4.3 MB of data it is bookkeeping for, and a large share of B2's compaction
load. Note the tension with K2: every map added to buy bucket headroom adds
directory traffic here, so the two findings pull in opposite directions and the
application has to trade them off with no visibility into either.

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
caller holding seventeen separate roots would not expect from the header.

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

**What this app did about it:** originally, a `BUILD_ASSERT` replicating
kvhash's private formula in application source. That is now **deleted** — the
app passes `initial_capacity = SIZE_MAX` to mean "as large as you can build",
which is what it wanted all along, and the clamp in (b) is what makes that
expressible. See X1 row 5. The formula never needed to be in the application;
only the *intent* did, and the API had no word for it.

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

### K11 — The directory is re-read from flash on every single operation (major, **`measured`**)

`kvhash_get`, `kvhash_set` and `kvhash_del` all open with `dir_load(root, &n)`,
which is a `blob_db_get(root)` — and by B1 that is a **whole 64 KB sector read**
before the operation has even located its bucket.

> `kvhash.c:117-143` (`dir_load`), called at `:219`, `:265`, `:339`

So **every map operation costs at least two `blob_db` operations**, and the
first of them exists only to look up an integer. That is the whole reason a map
get is 34.9 ms rather than 17.5 ms.

**Measured:** the `check` path performs exactly 4 blob operations per decision
and `byid` exactly 2 (`RESULTS.md` §4). Half of every one of those — **128 KB of
the 256 KB an access decision moves** — is spent learning two bucket ids.

The obvious reply is to cache the directory, and P3 ("no caches") is why it is
not. But the tension is narrower than it looks: the directory is 4 KB, is
immutable except when a bucket is *created*, and there are only as many of them
as there are open maps. `kvdb_t` already keeps `{root, ops}` in RAM across
calls, so keeping the directory beside it is the same pattern at a larger
constant — 4 KB per map, 68 KB for this app's seventeen.

The derived-bucket-id design in K1 makes the argument moot instead of winning
it: with `bucket_id = base_id + hash % n_buckets` the per-map state is
**16 bytes**, small enough to live in the handle without anyone calling it a
cache, and the directory read disappears. **A map get becomes one sector read
instead of two — R-D's cost halves**, with no denormalization, no index, and no
change to the on-flash format beyond the root record's own layout.

This is the strongest single conclusion in this register: `MAX_BUCKETS` (K1),
the directory rewrites (K5), and half of every read (K11) are all the same
decision — storing bucket ids that could have been computed.

---

## L1½ — `rootreg`

### R1 — `ROOTREG_MAX_ROOTS` defaults to 8 (minor, `read`)

Eight registered roots is below what a sharded dataset needs if each shard is a
named `kvdb` instance — the seventeen-instance L3 variant in `DESIGN.md` §12 does not
fit the default. The app's L2 layout uses one entry, so it never trips this, but
it is a sharp edge for the L3 route: the failure is `-ENOSPC` from
`kvdb_open`, at a layer that says nothing about registry capacity.

**A default, not a limit** (re-checked, `DESIGN.md` §12.1). `ROOTREG_MAX_ROOTS`
is `range 1 1000`, and the `BUILD_ASSERT` that bounds the registry image to one
payload allows 255 roots at `BLOB_DB_MAX_PAYLOAD_LEN=4096`. Seventeen instances
is one `prj.conf` line. That keeps this a finding — a default that silently
sizes the registry for a use case smaller than the one the layer above enables,
reported as `-ENOSPC` mid-open — but it is not the reason the app is on L2, and
`DESIGN.md` no longer claims it is.

---

## L3 — `kvdb`

The app does not link `kvdb` (`DESIGN.md` D10). These come from evaluating it as
the implementation route and rejecting it, which is itself a finding about L3's
current reach.

### V4 — Nothing composes shards, so L3 cannot hold a low-latency dataset (major, `read`)

K1 makes sharding mandatory past ~2.09 MB, and `kvdb` offers no help: the
application invents instance naming, the key→shard hash, read fan-out and
per-shard capacity accounting regardless. What `kvdb` still charges for that is
seventeen registry entries, seventeen meta blobs, thirty-four sector reads at boot, and a
collision with R1's default — in exchange for a naming feature a sharded app
does not need, because it has one superblock and knows its own shards.

L3's current audience is therefore **small stores of a few named instances**.
Every application larger than one map drops to L2, as this one does.

**Re-stated after the payload cap moved** (`DESIGN.md` §12.1). Two of those
charges have shrunk to nothing: B1 made the thirty-four reads slot walks rather
than sector reads (≈62 ms once, at §5's fitted cost), and R1's default is a
`prj.conf` line. The `~2.09 MB` threshold in this finding's title is also
obsolete — at the 32 746 B payload the DK's geometry now sustains, one map
reaches 4 092 buckets, and this app's 3.6 MB dataset fits in **one** `kvdb`.

The finding survives the correction and gets sharper, because sharding turns
out not to be a capacity workaround at all. `kvhash`'s directory must fit one
payload (K1), so bucket count and bucket size are one knob, and a map get reads
one of each (K11). Sixteen maps of 511 small buckets move **4 756 B** per get;
every single-instance layout that holds the same data moves **11 458–33 748 B**,
because it has to fatten the directory or the buckets and both are on the read
path. So the reason to shard is *latency*, and the reason `kvdb` cannot serve
this app is that it cannot put sixteen maps behind one name — not that the app
outgrew a map.

> That makes the proposed L3 addition more valuable, not less: a sharded-map
> interface would not be relieving a capacity ceiling, it would be owning the
> fan-out that keeps a lookup cheap.

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

### B11 — `CONFIG_BLOB_DB_PARTITION_LABEL` is documented and never read (moderate, `read`)

```
config BLOB_DB_PARTITION_LABEL
	string "Storage partition label"
	default "storage"
	help
	  Label of the fixed flash partition where blobs are stored. Must
	  refer to a fixed-partitions child node in the device tree.
```

Nothing in `lib/` or `include/` reads it. The flash backend hardcodes
`PARTITION_ID(storage_partition)` and the UBI backend uses
`FIXED_PARTITION_ID(BLOB_DB_UBI_PARTITION)`; a grep for the symbol outside
Kconfig returns nothing.

Setting it therefore does exactly nothing, silently — and it is precisely the
knob an application would reach for when trying *not* to hardcode a partition
name (X1 row 7). A dead option is worse than a missing one: it advertises a
capability that does not exist and rewards the careful caller with a wrong
answer.

Direction: honour it, or delete it. Either is fine; documenting it while
ignoring it is not.

### B12 — Compaction lowers the durable id ceiling (moderate, **`measured`** — filed as [issue #14](https://github.com/wmilek/key_value_db/issues/14))

Every boot after the first prints:

```
<wrn> blob_db: bucket scan out-ran ceiling (545 >= 539); raising
```

Four consecutive boots of this app's store, `native_sim`, 200 persons:
no warning, then `545 >= 539`, `568 >= 567`, `590 >= 581`. It is the steady
state, not an anomaly — and it reproduces with this application's changes
stashed, so it is not ours.

`next_id_hint` in the master header is written by two paths that disagree about
what it means. `alloc_id()` writes `next_id + 1 + 256` — a ceiling running ahead
of the allocation pointer, so master writes can be batched. The compaction
commit writes `st.next_id` — the pointer itself — which *lowers* the durable
ceiling and does not refresh `st.next_id_hint` in RAM. Allocation then compares
against the stale higher RAM copy and never re-persists, so ids get handed out
above the durable ceiling until the next mount notices.

That contradicts `doc/impl/l1_bucketlog.md` §13.1 ("an exclusive upper bound on
every id ever returned by `alloc_id` … the bucket scan only ever *raises* it,
never lowers it") and `blob_db.c`'s own comment at `alloc_id` ("the id we hand
out must be strictly below a persisted ceiling"). In the run above, id 545 was
bound while the persisted ceiling was 539.

**Why it is not corrupting anything, and why it still matters.** Mount's
defensive scan finds the true maximum *bound* id and raises the ceiling. But
§13.1 says the ceiling exists to protect *allocated-but-unbound* ids across a
crash — exactly what a scan of bound ids cannot see. Latent today only because
`kvhash` binds a bucket blob before publishing the directory entry that
references it.

Two smaller costs: every mount after a compaction pays an extra master write,
which erases a whole 64 KB PEB to write 64 bytes (~1.1 s on the DK by
`RESULTS.md` §5's measured erase — not confirmed on hardware, the DK captures do
not show the warning). And a detector written for "a corrupt or rolled-back
master" now fires on every ordinary boot, so a real one would be
indistinguishable.

Recorded, not fixed: `blob_db` is not this application's layer (`DESIGN.md` §2),
the same rule that parks B2. The direction is in the issue.

## X1 — An application cannot ask the stack anything, so it restates it (major, `hit`)

Not a new defect — it is B3, K9 and K10 seen together, from the position of
someone writing a client. Every question this application legitimately needed
to ask has no API, so the answer had to be copied out of a `.c` file and kept
in step by hand.

The full inventory, and what closes each:

| # | What the app must know | Where it really lives | Status |
|---|---|---|---|
| 1 | slot overhead = 14 B | `blob_db_internal.h` | **removed** — `main` checks it at mount |
| 2 | bucket header = 16 B | `blob_db_internal.h` | **removed** — same |
| 3 | sustainable payload = `(peb−16)/2 − 14` | `blob_db.c` | **removed** — same |
| 4 | `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` as a proxy for sector size | Kconfig | **removed** — same |
| 5 | `MAX_BUCKETS = (MAX_PAYLOAD−8)/8` | `kvhash.c:47` (private) | **removed** — see below |
| 6 | per-entry framing = 4 B, and the key length | `kvhash.c:291` | **still restated** |
| 7a | sector size | `blob_db`'s `struct st` | **removed** — `blob_db_iostats_get()` measures what it was modelling |
| 7b | partition size | `blob_db`'s `struct st` | **still fetched behind its back** — and see below |

Five of seven have now left the application — and note *how* each went: not by
exporting the constants, but by **removing the caller's need to ask**.

**Row 5 was never needed at all.** The app restated the bucket-count formula
for exactly one purpose: to pass `initial_capacity` at create time, so that
each map would be as large as possible. But the intent is "give me the largest
map you can build", and `buckets_for()` clamps any over-large request down to
capacity — so `initial_capacity = SIZE_MAX` expresses it precisely, and the
application never learns the formula. Same 511 buckets, same 51.6 % fill, same
zero overflows, one fewer private constant.

That reframes **K9(b)**. A silent clamp is a defect when you ask for a specific
size and quietly get less. It is also the mechanism that makes "as much as
possible" expressible, at a layer offering no other way to say it. Both are
true, and the second is worth keeping if the first is ever fixed: an explicit
`MAP_CAPACITY_MAX` sentinel would preserve it while letting a *specific*
over-large request fail loudly.

The same idiom already covers `blob_db_prepare()`, which caps at the bucket
total — the app passes `(size_t)-1` and never needs blob_db's bucket count
either. **So neither proposed `info` struct should carry `n_buckets`: nothing
above needs it, and offering it would invite exactly the restating this finding
is about.**

### Row 7 in detail — the leak that returns a wrong answer

Rows 1–6 were duplication: the app knew something it shouldn't, but it knew it
*correctly*. Row 7 is different, because the workaround can be silently wrong.

The app calls `flash_area_open(PARTITION_ID(storage_partition))` and reports
`fa_size` as the denominator of "51.6 % of the partition" — the whole of R-E.
Three things are wrong with that, none detectable from where the app stands:

1. **It hardcodes the partition, because so does the library.** The flash
   backend does `#define BLOB_DB_PARTITION_ID PARTITION_ID(storage_partition)`
   (`blob_db_store_flash.c:20`). An application trying to be careful would
   instead honour `CONFIG_BLOB_DB_PARTITION_LABEL` — which is documented,
   defaults to `"storage"`, and **is read by nothing at all** (B11). Doing the
   apparently-correct thing is the way to get this wrong.

2. **On the UBI backend the answer is simply false.** With
   `CONFIG_BLOB_DB_BACKEND_UBI`, `blob_db_store_ubi.c` reports
   `peb_size = info.leb_size` — a UBI *logical* erase block, smaller than a
   physical one — over `FIXED_PARTITION_ID(BLOB_DB_UBI_PARTITION)`, a
   **different devicetree node**, with `BLOB_DB_UBI_SPARE_PEBS` held back for
   wear-levelling. So the app would open a partition the store is not on,
   and divide by a capacity UBI never offers it. The percentage would be
   confidently, quietly wrong.

3. **It duplicates an invariant it cannot check.** `blob_db` verifies that every
   sector is the same size and that the partition divides evenly by it
   (`blob_db_store_flash.c:60-70`). The app read `sectors[0]` and assumed.

The third is now moot: the sector size was only ever fetched to *model* flash
traffic as `operations × sector size`, and `blob_db_iostats_get()` measures
that directly, so it is gone (row 7a). **The leak shrank because a real API
replaced a guess** — which is the same lesson as rows 1–5 arriving from a
different direction.

What remains is one number, `partition_bytes`, and one honest statement of the
problem: **an application cannot ask the store how big it is, so it asks
something else and hopes they agree.** On one of the two shipped backends, they
do not.

**What would close 6 and 7b.** Two small, read-only calls, neither requiring an
on-flash format change, and both narrower than the sketch this finding
originally carried:

```c
/* L1 — what blob_db already computed in struct st, and nobody can see. */
struct blob_db_info {
        size_t partition_bytes;  /* for "how full am I, as a fraction?" */
        size_t sector_bytes;
        size_t max_payload;      /* what this geometry can actually rebind */
};
void blob_db_get_info(struct blob_db_info *out);

/* L2 — what a map holds, not how it is built. */
struct map_info {
        size_t entries;          /* K10 */
        size_t entry_overhead;   /* the 4 B of framing — row 6 */
        size_t max_value;
        size_t fullest_bucket;   /* how close K2's cliff is */
};
int (*info)(uint64_t root, struct map_info *out);   /* new op on map_ops */
```

Both describe *what the store contains or can hold*, never how it is
structured. `blob_db_get_info()` is pure accessor work over state `mount()` has
already computed and deletes row 7. `map_ops.info()` deletes row 6, closes K9
and K10, and would turn `tools/sizing.py` from an offline script the build
cannot check into a runtime assertion.

**Why it matters beyond tidiness.** A restated constant is not merely ugly — it
is a silent-divergence bug waiting for the layer below to change. This app
restated four of them and `main` then changed the layer below; nothing warned,
because nothing could. They happened to still be right. The fifth (row 5) is
one Kconfig edit away from being wrong by 8×, and would surface as `-ENOSPC`
hours into a fill (K9).

## N1 — Fewer bytes, far more transactions (moderate, `measured`)

`main`'s slot-header walk cut the bytes an access decision moves by 19× (B1).
It raised the number of flash *operations* sharply in the same move: **261 read
transactions per access decision**, against 4 before.

> `RESULTS.md` §4: `check` = 200 ops → 52 176 flash operations, 2 654 060 bytes

On `native_sim` that is free — a transaction is a `memcpy`. On the DK it is
not: each SPI transaction carries a fixed command-and-address cost before any
data moves, so 261 small reads do not cost 1/19th of 4 large ones. `blob_db`'s
own iostats documentation makes exactly this point — *"a change that reads fewer
bytes may issue more transactions … judging such a change on bytes alone
flatters it"* — and this application is now the workload that makes the caution
concrete.

Whether the trade is a net win on hardware is **the single most valuable thing
the pending DK run can answer**, and it cannot be answered from `native_sim` at
all. Both numbers are in `RESULTS.md` so the comparison is possible once the
board numbers exist.

The board now supplies the constants to bound it: a 64 KB read is 28.8 ms
(0.45 ms/KB), so 13.3 KB of data is ~6 ms, and 261 transactions at 5–20 µs of
fixed cost add 1.3–5 ms. **`check` should land at 7–11 ms against 114.2 ms
measured pre-merge** — but the width of that range *is* the finding, and only
the board closes it.

### Closed by the board: a transaction costs ~65 µs

Measured post-merge at `338ec24` (`RESULTS.md` §5): **`check` = 14.605 ms**,
with **112** flash transactions and 10 030 B per decision.

| | assumed | measured |
|---|--:|--:|
| transactions per `check` | 261 | **112** |
| fixed cost per transaction | 5–20 µs | **~65 µs** |
| `check` | 7–11 ms | **14.605 ms** |

**The trade is a clear net win on hardware** — 114.2 ms → 14.6 ms, a 7.8×
improvement — so the slot-header walk was right and B1's fix stands. But the
caution this finding recorded was justified in substance: fixed transaction cost
is **54% of the remaining decision cost** (112 × 65 µs = 7.3 ms of 13.7 ms
non-CBOR), against the 12–36% the 5–20 µs guess implied. Bytes no longer
dominate; the two terms are comparable, and transactions are the larger.

That sharpens rather than retires the finding. The original concern was that a
smaller-sector part or a slower bus could land on the other side of the trade.
With transactions already the majority term on *this* part — 64 KB sectors on a
comparatively fast Quad-SPI — that margin is thinner than the pre-merge numbers
suggested. A part with smaller sectors moves fewer bytes per transaction and
would shift the balance further, so **a transaction-count budget is worth
carrying forward into any v2 read path**, not just a byte budget.

**One caveat on the headline number, and it matters.** The 14.605 ms was
measured at **1 000 persons**, not the benchmark's 10 000 (`RESULTS.md` §5).
Transaction count scales with the dataset: `RESULTS.md` §3b measures 112
transactions per `check` at 1 000 persons and **261** at 10 000, on the same
build. Apply the 65 µs established above to 261 and full-scale `check` lands
near **24 ms**, not 14.6.

So the 7.8× improvement over the pre-merge board is real and measured at equal
scale; the absolute figure is not yet the benchmark's figure. A4 stays open
until a 10 000-person board run says otherwise.

An earlier version of this paragraph explained the 261 → 112 difference as the
effect of `blob_db`'s one-entry index cache. That was wrong twice over: the
cache is compiled only under `CONFIG_BLOB_DB_LARGE_PAYLOADS`, which this app
does not enable, and the difference needed no mechanism at all — it is two
dataset sizes. The correction is worth keeping visible, because the wrong
version made a scale artefact look like a resolved improvement, which is
exactly the error that would have let a 1 000-person number stand in for a
10 000-person one.

## Not yet exercised

Findings that need the running app before they can be stated honestly:

- whether the §6.1 sizing rule holds — a single `-ENOSPC` during a full fill
  means it did not, and the margin needed is larger still (F11 counts them,
  A8 asserts zero);
- whether the fill matches the ≈ 2.2 h estimate, and how much of it is B2;
- compaction behaviour and latency spikes at ~50 % occupancy;
- whether CBOR encode/decode is measurable at all next to B1 (expected: no);
- whether K7's shared scratch buffers cause any surprise with seventeen live maps;
- `native_sim` versus DK divergence, which would be a finding about the
  `native_sim` model rather than about the stack.
