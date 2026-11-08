# Design change proposal — `kvhash`: inline on insert, detached on update

Status: **draft, implemented behind a Kconfig (default off)** · 2026-11-07
· Target implementation: `lib/containers/kvhash/kvhash.c`
· Builds on: `doc/proposals/2026-10-09-kvhash-spill.md` (entry format v3)
· Governed by `doc/principles.md`

**The ask.** A `set` that replaces a value rewrites the key's whole bucket. In a
map whose keys are rewritten often, almost every byte written is a neighbour's
unchanged entry. The idea: the first insert stores the value inline, as today;
the first *update* moves it into a blob of its own; and every later update of
that key writes only that blob. A key that is never rewritten costs exactly what
it does now.

**Short answer.** It works as intended on the write side: on a recorded
rewrite-heavy soak, bytes written fall 8.4× and erases 7.6×. It moves the cost to
reads: a get of a detached value is one more blob lookup, and blob_db buckets
fill with many small slots. Without blob_db's slot index, or with one too small
for the live ids, lookups get 1.8–2.8× more expensive in reads, and on large
erase blocks reads are most of the flash time. With an index that covers the live
ids, the modelled flash time falls 2.6×. So the policy is worth having only
together with the slot index, and only for values that are not tiny (§6).

---

## 1. Entry format (v4)

One new encoding in the v3 entry, no new field:

```
[u16 klen | KS] [u16 vlen | VS] [key part] [value part]

VS = 1, vlen = 0 : DETACHED value   value part = [u64 val_id]
                   the true length is the blob's length
VS = 1, vlen > 0 : spilled value (v3), vlen is the true length
```

- **Why no length.** If the entry recorded the length, an update that changes it
  would have to rewrite the bucket as well, and on a crash between the two
  writes the entry and the blob would disagree. With the length kept only with
  the bytes, every update of a detached value is one atomic `blob_db_update`,
  whatever its length.
- **Why the encoding is free.** v3 spills a value only when `vlen >
  VAL_INLINE_MAX >= 0`, so `VS = 1, vlen = 0` is never written by v3 code.
- **Why it still needs a version.** v3 firmware would read it as a spilled empty
  value and fail the length check with `-EIO`: not a wrong answer, but an
  unreadable key. So a map that may hold detached entries says **v4** in its
  directories, and v3 firmware already refuses a version it does not know. A map
  is created v4 only while the policy is on; with it off, `create` still writes
  v3, so a build that never enables the policy changes nothing on flash.

## 2. Write policy

As with the spill thresholds, this is a policy read only when `set` builds an
entry. Reads go by the entry alone.

| old entry | new value | what `set` does | flash writes |
|---|---|---|---|
| none (insert) | any | placed by the thresholds, as today | bucket (+ directory if fresh) |
| inline | `>= DETACH_MIN_LEN` | bind a fresh value blob, rewrite the bucket with an 8 B reference | 2 |
| inline | shorter | rewrite the bucket inline, as today | 1 |
| detached | any length | `blob_db_update(val_id)` | **1** |
| spilled (v3, sized) | same length | `blob_db_update(val_id)`, as today | 1 |
| spilled (v3, sized) | other length | fresh blob, bucket now points at it detached, old blob released | 2 + release |

- **Sticky.** A detached value stays detached, whatever length it is set to, so
  a key that has been rewritten once keeps the one-write path.
- **`DETACH_MIN_LEN`** (default 16 B). A short value stays inline on update. Its
  8 B reference saves next to nothing in the bucket, and a detached value costs a
  blob read on every get (§5).
- **Off again** (`kvhash_set_detach_on_update(false)`): the next `set` of a
  detached key places its value by the thresholds and releases the blob after
  the commit. Nothing is migrated eagerly.
- **v2/v3 maps** are served as before and never get detached entries, whatever
  the policy.

## 3. Reads

- `get`: a detached value is one `blob_db_get` into the caller's buffer. On
  `-ENOMEM` the length comes from `blob_db_size`, so the caller still learns it.
- `next`: reports the value length before it checks buffer sizes, so a detached
  entry costs a `blob_db_size` there.
- `count`, `has`, the order of `next`: unchanged. None of them reads values.

## 4. Crash consistency

The spill design's argument applies unchanged. Blobs are bound before the bucket
that names them and released after it, and every step is one atomic blob_db
write:

- **Inline → detached:** a cut before the bucket commit leaves the old inline
  value and at most one unreferenced value blob.
- **Detached → detached:** one write. Old or new, nothing leaked.
- **Detached → inline (policy off), delete:** commit, then release. Old value or
  new, and at most the old value blob leaked.

`tests/lib/containers/src/spill_crash.c` cuts power after every one of kvhash's
writes in five new scenarios (inline value becomes detached; detached rewrite at
the same and at a new length; detached back inline; delete detached). After each
cut, the key is wholly old or wholly new and leaks stay within those bounds.
`src/detach.c` (suite `kvhash_detach`) covers the policy:

- one write per update across length changes, down to an empty value;
- short-buffer lengths from `get` and `next`;
- v3 maps never detached;
- policy off re-inlines and releases;
- remount;
- conversion of a v3-spilled value;
- `DETACH_MIN_LEN`.

The whole containers and kvdb suites also pass with the policy forced on at
`DETACH_MIN_LEN = 0`.

## 5. Measured

The workload is a recorded 16.9 h soak of one two-level map (16 × 16 leaf
buckets, declared 5,000 entries of about 139 B):

- about 4,800 keys;
- 133 k sets, 128 k of which replace a value (89 % at the same length; values
  71 B on insert and 107 B on update, on average);
- 1.96 M gets;
- 14 k log appends.

The store is UBI on an 8 MiB partition with 64 KiB erase blocks and a 16 KiB
payload cap, replayed on native_sim. Every run replays all 2.1 M calls and ends
with the recording's map content and iteration order. Flash time uses the
nRF5340-DK model of `app_perf_l0/RESULTS.md` §5 (read 65.7 µs + 0.258 µs/B,
write 157 µs + 12.16 µs/B, erase 58 ms per 4 KiB):

| | bytes written | erases | compacted bytes | blob_db reads | modelled s (read / write / erase) |
|---|--:|--:|--:|--:|--:|
| today, no slot index | 186.3 MB | 2,935 | 27.4 MB | 636 M | 49,503 (44,490 / 2,289 / 2,724) |
| detach, no slot index | **22.2 MB** | **387** | **1.9 MB** | 1,793 M | 124,333 (123,678 / 295 / 359) |
| today, slot index 4,096 | 186.3 MB | 2,935 | 27.4 MB | 100 M | 12,635 |
| detach, slot index 4,096 | 22.2 MB | 387 | 1.9 MB | 313 M | 22,522 |
| today, slot index 32,768 | 186.3 MB | 2,935 | 27.4 MB | 19.1 M | 7,049 (2,036 / 2,289 / 2,724) |
| detach, slot index 32,768 | 22.2 MB | 387 | 1.9 MB | 25.0 M | **2,694** (2,040 / 295 / 359) |

Why reads grow:

- **One more lookup per get.** With an index this is visible on its own: +31 %
  read operations. Read *bytes* halve, though, because each get now reads a
  bucket of about 560 B instead of about 2.6 KB. Read time comes out equal.
- **Denser blob_db buckets.** Each update appends a ~120 B slot instead of a
  ~2.6 KB one, so a 64 KiB blob_db bucket holds ~20× more slots. Without an
  index every lookup walks slot headers 12 B at a time, and that cost lands on
  every id: directories, buckets, values and logs.
- **A full index.** 4,096 entries cannot hold this store's live ids (about
  11.7 k today, 16.4 k detached). The index does not evict, so the detached
  values, created late, are mostly walked.

An insert-only workload (2,000 inserts into two maps, then lookups) replays
identically, counter for counter: no update, no detach. With unchanged re-sets of
its ~7 B values repeated, detaching every update (`DETACH_MIN_LEN = 0`) saves
13–17 % of bytes written and 13–16 % of erases, but costs 1.2–2× the reads. The
modelled time is 4–20 % worse on the re-set phases and 41–67 % worse on the
lookup phases (UBI, 4 KiB erase, a 2,048-entry slot index that is full). That is the
case `DETACH_MIN_LEN` exists for. At the 16 B default those values stay inline:
the repeats then match today's counters exactly, and a variant that deletes and
re-inserts the population comes within 0.3 % (one longer record detaches). The
soak's numbers above are the same at 0 and at 16 B: every update there is longer.

## 6. Recommendation

- **Keep it opt-in.** It changes the format (v4) and trades reads for writes.
  Whether that trade pays depends on the slot index and the read/write ratio.
- **Enable it with `CONFIG_BLOB_DB_SLOT_INDEX`, sized for the live ids.** On the
  measured soak that is a 2.6× cut in modelled flash time, 7.6× fewer erases
  (wear), and 15× less compaction copying.
- **Do not enable it without the index on large erase blocks.** There the read
  cost dominates, and it more than doubles.

## 7. Open items

- **O1** Slot index eviction, or a bound on it. The win above needs an index
  covering every live id (12 B each). A replacement policy would let a smaller
  index serve the hot set.
- **O2** A per-map policy in `map_config`, so a rewrite-heavy map can detach
  while an insert-only one on the same store does not pay for the v4 format.
- **O3** The orphan sweep of the spill proposal (its O1) now also covers detached
  values: a cut between bind and commit leaks one.
- **O4** A DK measurement of the extra lookup, and of the 64 KiB block erase,
  which the model overstates as 16 × 4 KiB.
- **O5** `next` pays a `blob_db_size` per detached entry. A walk that does not
  want lengths could skip it, but the shape has no such call.
