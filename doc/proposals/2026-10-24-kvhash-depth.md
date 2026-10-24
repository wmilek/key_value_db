# Design change proposal — more than two levels for `kvhash`

Status: **proposed** · 2026-10-24
· Target contract: `doc/layers/l2_containers.md` §4.3; `include/app/lib/containers/shape_map.h`
  (`map_info` wording only)
· Target implementation: `lib/containers/kvhash/kvhash.c`
· Builds on: `2026-08-20-kvhash-second-level.md` §8 (depth as a parameter) and
  §11.2 (independent indices from one hash)
· Governed by `doc/principles.md`

**The ask.** A map declared at 2.4 million entries of 4 B is refused by
`create` with `-EINVAL` (`kvhash.c:726`), because two levels are the ceiling.
Nothing on flash imposes that ceiling: every directory is the same record and
its depth is a `u8`. The code does. What does it take to lift it, without
making a one- or two-level map any slower?

**Short answer.** Three parts, each landable on its own:

- **(A) Size buckets against the payload.** Today's rule plans buckets that
  cannot fit in one payload, so a deeper map built on it would overflow
  anyway. This is a bug fix independent of depth, and it comes first.
- **(B) One walker for every depth.** Every code path becomes a loop over
  levels instead of a depth-1 / depth-2 branch. No format change, still capped
  at two, and the same flash I/O at depths 1 and 2, proven by identical
  `CONFIG_BLOB_DB_IOSTATS` counters.
- **(C) Admit depths 3 and 4.** A new index formula used only at depth ≥ 3,
  interior directories created eagerly at `create` as depth 2 does today, and
  a hard cap.

Every existing map, and every population that two levels can hold, is built
and served exactly as it is today.

**What this does not do: make the data fit the flash.** 2.4 M × (4 B + 4 B
entry header) is 19.2 MB, against an 8 MB storage partition on the DK and on
`native_sim`. A deeper map raises the container's ceiling. The medium's
ceiling (`app_cbor_persondb/FINDINGS.md` K13, and plain partition size) is
untouched. §5 shows that for this particular population, a 2 KB payload with
part A alone already gives a working geometry. Depth is what small-payload
builds and much larger stores need.

---

## 1. Where the ceiling is today

| What stops a third level | Where |
|---|---|
| A directory with depth outside {1, 2} is refused as corrupt | `dir_load_raw()`, `kvhash.c:310` |
| `create` refuses a population two square levels cannot hold | `derive_geometry()`, `kvhash.c:726` |
| One CRC-32 is split into exactly two 16-bit indices | `idx_top()` / `idx_sub()`, `kvhash.c:236–244` |
| Every operation branches on depth 1, or else on depth 2 with depth-1 children | `resolve_leaf()`, `sub_load()`, `kvhash_stat()`, `tree_first_after()`, `kvhash_count()`, `kvhash_destroy()` |
| `create` builds exactly one or two levels | `kvhash_create()` |

A directory must fit one payload, so one level holds at most
`MAX_BUCKETS = (MAX_PAYLOAD − 8) / 8` children and two levels hold
`MAX_BUCKETS²` buckets:

| Payload | `MAX_BUCKETS` | Buckets at depth 2 |
|--:|--:|--:|
| 256 B (default) | 31 | 961 |
| 1 024 B (`app_perf_kvdb`) | 127 | 16 129 |
| 2 016 B (large-payload default) | 251 | 63 001 |
| 16 384 B (`app_cbor_persondb`) | 2 047 | 4 190 209 |

The ceiling bites on small-payload builds. At the default 256 B, a bucket
holds at most 32 entries of 8 B, so no declaration reaches much past 30 000
entries.

## 2. Part A — size buckets against the payload

The two-level branch of `derive_geometry()` sets
`buckets = min(entries, entries × typical / MIN_BUCKET_BYTES)`, floored at
`ONE_LEVEL_MAX_BUCKETS`. It has three defects:

1. **`MIN_BUCKET_BYTES` (4 096) is never capped by the payload.** At 256 B or
   2 016 B it plans buckets that hold more than one payload can. Example:
   2.4 M × 4 B at 2 016 B builds 49 × 49 buckets, about 1 000 entries each,
   roughly 8 KB against a 2 016 B limit. `set` starts returning `-ENOSPC`
   after about 480 k inserts, when the fullest bucket fills.
2. **`typical_entry_bytes` excludes the 4 B entry header** (`ENTRY_HDR_LEN`).
   It counts key + value only (`shape_map.h`), so for 4 B entries the real
   size is double the planned one.
3. **The margin is a fixed fraction, and overflow is not a fraction.** The
   fullest of B buckets sits about √(2 ln B) standard deviations above the
   mean, so the risk depends on how many entries a bucket holds and on how
   many buckets there are. At 256 B with ~130 k buckets, a 60 %
   (`BUCKET_FILL_PCT`) target leaves about 180 buckets expected to overflow.
   At 2 016 B with ~16 k buckets, 60 % is more margin than needed.

Proposed rule, for the two-level (and deeper) branch only:

```
entry_bytes  = typical + ENTRY_HDR_LEN
cap_entries  = MAX_PAYLOAD / entry_bytes            what one bucket can hold
lambda       = largest mean with  lambda + K * sqrt(lambda) <= cap_entries
bucket_bytes = min(MIN_BUCKET_BYTES, lambda * entry_bytes)
buckets      = max(ONE_LEVEL_MAX_BUCKETS,
                   min(entries, entries * entry_bytes / bucket_bytes))
```

- `typical == 0` keeps today's meaning: about one entry per bucket.
- `K = 6.5` keeps the expected number of overflowing buckets below 0.01 for a
  Poisson fill at the populations below. The arithmetic is integer only and
  runs once, at `create`.
- `tools/sizing.py` carries the twin of the rule and checks it against real
  key sets, since `crc32_ieee` is `zlib.crc32`. That is the discipline
  second-level §13.1 established: a check must be the code's twin or it is
  checking something else.

| Population | Payload | Today | Part A |
|---|--:|---|---|
| 2.4 M × 4 B | 256 B | `-EINVAL` (needs 49 per level, limit 31) | 225 882 buckets: needs depth 4 (part C) |
| 2.4 M × 4 B | 1 024 B | 49 × 49, `-ENOSPC` at ~0.22 M | 33 103 buckets: needs depth 3 (part C) |
| 2.4 M × 4 B | 2 016 B | 49 × 49, `-ENOSPC` at ~0.48 M | **120 × 120, depth 2: fits** |
| persondb people, 10 000 × 380 B | 16 KB | 31 × 31 | 31 × 31 (the 4 096 B floor still binds) |
| persondb people, 8 000 × 380 B | 16 KB | 28 × 28 | 28 × 28 |
| persondb credentials, 25 000 × 23 B | 16 KB | 16 × 16 | 16 × 16 |
| `app_perf_kvdb`, 768 keys | 1 KB | depth 1, 192 buckets | unchanged (one-level branch untouched) |

No shipped application's geometry changes. Geometry is recorded in the map, so
part A affects only maps created after it lands. It needs no format change.

## 3. Part B — one walker for every depth (refactor, behaviour unchanged)

Generalise the code paths while still refusing depth > 2:

- **What a directory's depth means.** A directory records how many directory
  levels lie at and below it: the root holds `d`, its children `d − 1`, leaf
  directories `1`. Depth-2 maps already store exactly this (their sub-maps
  record 1), so no stored byte changes. Validation generalises today's
  `sub_depth != 1` to `child.depth == parent.depth − 1`.
- **`descend()` replaces `resolve_leaf()`.** It loads a directory into
  `dir_buf`, computes that level's index, takes the child, and repeats until
  it reaches depth 1. On the way it records a path of `(id, n, index)` per
  level on the stack.
- **One iterative walker serves `next`, `count` and `destroy`**, in order for
  the first two and post-order for `destroy`. When it leaves a subtree it
  re-reads only the parent, from the path. That is what `top_child()` does at
  depth 2 today, so depths 1 and 2 issue the same blob_db calls in the same
  order.
- **`stat()` reads the leftmost path**, as it reads `sub[0]` today.
- **`create()`** uses part C's top-down build (§4.3), which at depth 2 issues
  today's writes.

**Acceptance is the "no slower" guarantee made checkable.** Record the
`CONFIG_BLOB_DB_IOSTATS` counters (reads, writes, bytes) of every operation in
`tests/lib/containers`, `app_perf_kvdb` and `app_cbor_persondb` before the
change. After part B they must match exactly.

A doc correction belongs here too. `doc/impl/l2_kvhash.md` §3.4 says `destroy`
copies the sub-directory ids into `bkt_buf`. Since spilling
(`2026-10-09-kvhash-spill.md` §9) the code re-reads the top directory for each
sub-map instead, because `bkt_buf` holds each bucket while its spilled blobs
are released. The generic walker does what the code does; the document should
say so.

## 4. Part C — depths 3 and 4

### 4.1 Index: one hash, mixed radix

Depths 1 and 2 keep today's formula. Changing it would misplace every key
already stored. At depth `d ≥ 3`, with a uniform fan-out `F` and
`B = F^d ≤ 2^24` buckets:

```
b   = crc32_ieee(key) % F^d
i_k = (b / F^(d-k)) % F          k = 1 (top) .. d (bucket within its leaf)
```

- **The indices are uniform and independent.** They are the digits of `b` in
  base `F`, uniform because `b` is, and independent for the same reason. That
  is not two moduli of one value, which is the failure second-level §11.2
  warns about.
- **The modulo bias is at most `F^d / 2^32`**, which is 0.4 % at the cap.
- **The enumeration order stays a function of the key and the geometry.**
  The tuple `(i_1, …, i_d)` sorts exactly as `b` does, so `next`'s order
  `(i_1, …, i_d, klen, key_fp, key bytes)` keeps that property, and a deleted
  or never-stored key still has a successor.

### 4.2 Geometry

- `d` is the smallest depth ≥ 2 with `MAX_BUCKETS^d ≥ buckets`.
- `F = ceil(buckets^(1/d))`, the same at every level, which is today's square
  rule generalised.
- **Refused with `-EINVAL`, nothing written** (as today): `d > KVHASH_MAX_DEPTH`
  (4), or `F^d > 2^24`. The bucket cap keeps the bias bound above and keeps
  `map_info.buckets` (a `u32`) exact. It sits just above today's own maximum,
  4 091² = 16 736 281.

**Depth policy: the fewest levels that fit.** No population that two levels
can hold is given three, which is what keeps every existing geometry.
Second-level §8 found depth 3 faster from about 65 000 buckets even where
depth 2 fits. Choosing depth for latency is a separate decision (O2).

At depth 2, `n_top` and `n_sub` stay separately recorded (square in practice,
as today). At depth ≥ 3 every directory's `n` must equal the root's, and a
mismatch is `-EIO`.

### 4.3 Interior directories: eager, as at depth 2

Every directory is written at `create`. Afterwards only leaf directories are
ever rewritten, when a fresh bucket is published into one. This is the
property that let depth 2 add no new crash-consistency work (the `kvhash.c`
header), and it carries over unchanged. Readers keep treating a zero id in an
interior directory as corruption (`-EIO`).

**Create builds top-down with the two existing buffers.** Allocating an id is
a RAM operation, and an allocated id may be named before it is bound (the id
lifecycle in `blob_db.h`). So:

- each interior directory is written complete as soon as its children's ids
  are allocated;
- the root's image is held in `dir_buf` and written last, as the commit point
  (unchanged);
- to move on to a sibling, the parent is re-read into `bkt_buf`.

That costs one extra read per interior directory, at `create` only. At depth 2
there is no interior level below the root, so the writes are today's: `F`
sub-maps, then the root.

The cost is `1 + F + … + F^(d−1)` directories written at `create`:

| Population | Payload | Geometry | Directories | Directory bytes |
|---|--:|---|--:|--:|
| 2.4 M × 4 B | 256 B | depth 4, 22⁴ = 234 256 buckets | 11 155 | ≈ 2.2 MB |
| 2.4 M × 4 B | 1 024 B | depth 3, 33³ = 35 937 buckets | 1 123 | ≈ 0.32 MB |

A power cut during `create` leaks all of them until a format. This is the B8
window (`app_cbor_persondb/FINDINGS.md`), paid once per map as today, but
larger. The lazy alternative is weighed in §6.

### 4.4 Format, and old firmware

**No version bump.** Depth 3 or 4 is a new value of an existing field, and
every firmware that reads a v3 map already rejects depth outside {1, 2} in
`dir_load_raw()` with `-EIO`. That is the same refusal the version check gives
a newer map, so old firmware never misreads a deep map. The bucket format is
untouched, and spilling works unchanged at every depth.

The cost of not bumping: old firmware cannot tell "too deep for me" from
"corrupt". It cannot tell "too new" from "corrupt" today either (D2).

### 4.5 RAM and stack

- No new static buffers.
- The path is at most 4 × (`u64` id + `u16` n + `u16` index), 48 B of stack.
  That is inside `l2_containers.md` §5's bounded-stack invariant.

### 4.6 Contract and documentation text

- `l2_containers.md` §4.3: "one or two levels deep" becomes "one to four",
  with the depth ≥ 3 index rule.
- `shape_map.h`: `map_info.fanout` ("slots in the top level") stays true,
  because fan-out is uniform at depth ≥ 3. `buckets` fits a `u32` under the
  cap.
- `doc/impl/l2_kvhash.md` §1, §1.1, §2, §3.4, §4 and §5: the depth bound, the
  geometry rule, the walker and the cost tables.

## 5. Costs

| Depth | Reads per `get` | Writes per `set` |
|--:|--:|---|
| 1 | 2 | 1 bucket (+ 1 directory if the bucket is new) |
| 2 | 3 | same |
| 3 | 4 | same |
| 4 | 5 | same |

- **A level costs a read, never a write.** On the DK's UBI backend that is one
  transaction (178 µs) plus `8 + 8F` directory bytes at 0.616 µs/B
  (second-level §13.2): at `F = 22` about 0.29 ms per lookup.
- **`next` and `count` read** every bucket and every directory, plus one
  parent re-read per subtree crossing.
- **`destroy`** costs the same walk plus one delete per blob.

**The whole 2.4 M × 4 B store, on flash** (entry data 19.2 MB; 14 B slot
header per blob; before blob_db's compaction headroom):

| Payload | Geometry | Mean bucket | Buckets + directories |
|--:|---|--:|--:|
| 256 B | depth 4, 22⁴ | 82 B | ≈ 22.5 MB + 2.2 MB |
| 1 024 B | depth 3, 33³ | 534 B | ≈ 19.7 MB + 0.32 MB |
| 2 016 B | depth 2, 120² | 1 334 B | ≈ 19.4 MB + 0.12 MB |

**Small payloads are a poor fit for large maps.** At 256 B the margin that
keeps 234 k buckets from overflowing leaves each about a third full, and the
directories add another 10 %. Every row needs a partition of roughly three
times the DK's.

## 6. Alternatives considered

- **A bigger payload instead of more depth.** It is bounded by the erase
  geometry (about 2 026 B on 4 KB sectors) and by K13 (the largest blob must
  fit an erase block's free space). Where it is available it is the better
  answer: §5's 2 016 B row needs no depth at all. It does not help
  small-payload builds.
- **Lazy interior directories.** `create` would write two blobs instead of
  thousands, with no large create-time leak window. But interior directories,
  the root included, become rewritable, so the write-once argument of §4.3 is
  lost. A zero interior id becomes legal, so corruption at those levels is no
  longer detected. And the first insert under an empty subtree costs up to
  `d − 1` extra writes, each its own crash window. Rejected for v1; revisit
  (O3) if large-declared, sparsely filled maps appear.
- **A 64-bit hash.** Unnecessary under the 2²⁴ bucket cap.
- **A third payload-sized static buffer**, to keep ancestors in RAM during
  `create` and walks. It costs up to 16 KB of RAM to save create-time and
  walk-time re-reads. Rejected.
- **Linear hashing / splitting** (second-level Option C). It solves growth,
  not depth, and part B's generic walker is a prerequisite for it.
  Second-level §8 said as much: "If 'two' is baked into the code paths, a
  third level is a rewrite."

## 7. Tests

**Part A**

1. `derive_geometry()` table tests: every population in §2's table yields
   exactly the listed geometry, the shipped applications included.
2. `tools/sizing.py` implements the twin of the new rule, reproduces `stat()`
   for `app_cbor_persondb` and `app_perf_kvdb`, and confirms fewer than 0.01
   expected overflowing buckets for 2.4 M × 4 B at 256 B, 1 024 B and 2 016 B.

**Part B**

3. Identical `CONFIG_BLOB_DB_IOSTATS` counters per operation, before and
   after, as §3 sets out.
4. The existing suites (`main.c`, `spill.c`, `spill_crash.c`, and the
   `slot_index` variant) pass unchanged.

**Part C** — a new testcase variant, `lib.containers.deep`, built with
`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=64` (`MAX_BUCKETS = 7`) so depths 3 and 4 are
reached with a few thousand entries on `native_sim`:

5. `get`, `set` and `del` at depths 3 and 4. `next` walks the whole map in the
   documented order and stays stable across `set` and `del` between calls.
   `count` is exact. `stat` reports depth, fan-out and bucket count.
6. `destroy` at depths 3 and 4 releases every blob, spilled parts included:
   the live-blob count returns to its baseline.
7. A power cut after each write of `create` (no reachable partial map, and a
   repeat builds a fresh one), of a `set` into a fresh bucket, and of
   `destroy` (a repeat completes, with no leak beyond the B8 window).
8. A population beyond depth 4 or 2²⁴ buckets is `-EINVAL` with nothing
   written, and a hand-built depth-5 directory is refused with `-EIO`.

## 8. Plan

1. **Part A**: the geometry rule, the `sizing.py` twin, and tests 1–2.
   Changes only new maps whose population is on the affected path; no shipped
   application's geometry changes.
2. **Part B**: the generic descent and walker, still capped at depth 2, and
   tests 3–4. A pure refactor, accepted on identical counters.
3. **Part C**: the depth ≥ 3 index, top-down `create`, the cap, and tests 5–8.
4. **Documentation**: §4.6, including the §3.4 correction.

## 9. Decisions this proposal needs from review

- **D1** The caps: `KVHASH_MAX_DEPTH = 4` and at most 2²⁴ buckets.
- **D2** No version bump (§4.4): the depth check already protects old firmware.
- **D3** Interior directories eager (§4.3) rather than lazy (§6).
- **D4** Part A's margin, `K = 6.5` standard deviations, replaces
  `BUCKET_FILL_PCT` on the two-level path. The one-level path is unchanged.
- **D5** Depth policy: the fewest levels that fit (§4.2).

## 10. Open items

- **O1** Orphan sweep (spill proposal O1). It would now also reclaim an
  interrupted deep `create`.
- **O2** A latency-driven depth choice: depth 3 where it beats depth 2
  (second-level §8, from about 65 000 buckets).
- **O3** Lazy interior directories, if large-declared, sparsely filled maps
  become common.
- **O4** A DK measurement of a depth-3 and a depth-4 lookup against §5's
  estimate.
- **O5** The medium's ceiling (K13, partition size) is outside this proposal,
  and for the 2.4 M × 4 B population it is the binding limit.
