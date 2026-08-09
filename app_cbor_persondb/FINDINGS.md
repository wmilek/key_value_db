# Findings — limitations of the stack, as seen from an application

Status: **v0.1 — seeded from code reading during design (2026-08-09).**
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

**Status values**

| | |
|---|---|
| `read` | derived from reading the code; not yet exercised |
| `hit` | the app actually ran into it |
| `measured` | quantified by a number in `RESULTS.md` |
| `wontfix` | understood, accepted, recorded for the record |

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

On the DK's MX25R6435F that is **64 KB of SPI traffic per operation**,
regardless of whether the payload is 5 bytes or 4 KB. It is the single cause of
every latency figure in this application: the measured 16.9 ms `blob_db` read
(`app_perf/RESULTS.md`) is 64 KB at ~4 MB/s, not a seek or an erase.

Impact here: resolving a credential to a person and checking a permission costs
2 `kvdb_get` = **4 sector reads = 256 KB** to answer a question about ~365 B of
data — read amplification of roughly **700×**.

Direction (not implemented): the format already stores per-slot lengths, so a
get could read the bucket header, then walk slot headers with short reads and
fetch only the matching payload. On NOR, random reads are cheap; the whole-sector
read buys simplicity, and the price is the entire performance envelope.

### B2 — Compaction costs five sector erases (major, `read`)

`compact_commit()` sequences: master ← COMPACTING, erase+write scratch, erase+
write bucket, erase scratch, master ← CLEAN. Each master write erases its sector
first. That is **five 64 KB erases** to compact one bucket, at ~1.1 s per erase
on the DK.

> `compact_commit()`; erase cost from `app_perf_kvdb/RESULTS.md` (`prepare`:
> 1.097 s per bucket)

Impact here: the ~37 MB appended during the fill drives ~1 250 compactions,
≈ 2 h of the ≈ 2.5 h one-time population.

Direction: two of the five erases are the double-buffered master, and one is a
post-hoc scratch erase that could be deferred to the next compaction's
erase-before-write.

### B3 — No occupancy or geometry introspection (major, `read`)

The public API exposes no partition size, sector size, bucket count, live bytes,
free bytes or fill level. An application cannot:

- size a dataset to a fraction of the medium (**this is R-E**),
- predict or pre-empt `-ENOSPC`,
- report how full it is,
- decide how many buckets to hand `blob_db_prepare()` (see B7).

Impact here: to report "50 % of the external flash" the app must read
`FIXED_PARTITION_SIZE(storage_partition)` from devicetree, going behind the
library's back and duplicating knowledge `blob_db` already has. It still cannot
observe physical occupancy — live bytes plus not-yet-compacted garbage — at all,
so the achieved fill can only be reported as *logical* bytes.

Direction: a `blob_db_stat()` returning geometry and live/free/garbage byte
counts. `mount()` already computes most of it into `struct st`.

### B4 — `count()` / `iterate()` are O(n²) (moderate, `read`)

Both are documented O(n²) in entry count, because determining whether a slot is
superseded rescans the rest of its bucket. At the ~4 300 blobs this app creates
they are a diagnostic that costs seconds, not a routine health check.

> `blob_db.h` @ref blob_db_count; `build_compacted_image()` for the same pattern

### B5 — `MAX_PAYLOAD_LEN` is Kconfig-capped at 4096 (moderate, `read`)

`config BLOB_DB_MAX_PAYLOAD_LEN … range 1 4096`. Nothing in the on-flash format
requires 4096 — a slot is `14 + val_len` inside a sector that is 64 KB on the
DK. The cap transitively limits a `kvhash` instance to ~2.09 MB (K1), which is
what forces this app to shard.

### B6 — 128 KB of `.bss` for sector buffers (moderate, `read`)

`g_bbuf` and `g_bbuf_new` are each `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` — 64 KB on
the DK, so **128 KB, a quarter of the nRF5340's 512 KB RAM**, permanently
resident. `kvhash` adds 2 × `MAX_PAYLOAD` = 8 KB of its own (K7).

This sits awkwardly beside principle P3 ("minimum RAM usage: O(1) steady-state
RAM per module, no caches"). It *is* O(1), but the constant is set by the flash
part's erase-block size, which is the one thing the application cannot change.

Direction: B1's short-read get would remove the need for one of the two.

### B7 — First write to a fresh bucket stalls ~1.1 s (moderate, `read`)

An `update` landing in a never-written bucket erases and headers that sector
in-line. `blob_db_prepare()` exists to move the cost off the hot path, but the
application must drive it — and cannot know how many buckets to prepare, because
bucket count is not exposed (B3). `app_perf_kvdb` passes `N_KEYS * 2` as a guess.

---

## L2 — `kvhash`

### K1 — An instance tops out near 2.09 MB (major, `read`)

The bucket directory must fit one blob payload, so
`n_buckets ≤ (MAX_PAYLOAD − 8)/8 = 511`, and each bucket's packed list must also
fit one payload. Instance capacity is therefore
`MAX_PAYLOAD × (MAX_PAYLOAD − 8)/8 ≈ 2.09 MB`.

> `kvhash.c:45-50`

Impact here: 10 000 person records (~3.6 MB) **cannot live in one `kvdb`
instance**. The app shards across eight named instances and implements its own
key→shard fan-out. Nothing in the API helps: naming, hashing, fan-out and
per-shard capacity accounting are all the application's problem (V4).

### K2 — Overflow is per-bucket, and depends on entry-size variance (major, `read`)

`kvhash_set` returns `-ENOSPC` when one bucket's packed list would exceed a
payload — while the store as a whole may be nearly empty. There is no split,
no rehash, no overflow chain. Safe load factor is therefore not a property of
the container but a statistical property of *the application's key hash and
value-size distribution*, which the application must work out for itself.

Concretely: the same 511-bucket instance holds 25 000 credential entries (23 B)
comfortably, but only ~5 600 person entries (362 B). Sizing the people shards
meant computing a Poisson tail by hand — see `DESIGN.md` §6.

Direction: bucket splitting, an overflow-blob chain, or simply reporting
per-bucket occupancy so an application can see the cliff coming.

### K3 — `n_buckets` is fixed at create time (major, `read`)

Chosen from `map_config.initial_capacity` on first create and never changed. A
store that outgrows its bucket count has no growth path short of rebuild — and
rebuild needs iteration, which does not exist (K6). Capacity must be planned
exactly, before the first insert, at a layer with no visibility into how much
flash exists (B3).

### K4 — Every `set` rewrites the whole bucket blob (major, `read`)

Insert, update and delete all read the bucket, edit it in RAM, and
`blob_db_update()` the entire packed list back. Write amplification is
`bucket_bytes / entry_bytes` — about 8–16× at the fill levels here — and it is
paid on *every* write, including a one-byte change.

### K5 — The directory is rewritten on every first insert into a bucket (major, `read`)

A bucket blob is created lazily; publishing it writes the whole directory back.
The directory is `8 + 8 × n_buckets` = **4 KB at 511 buckets**.

> `kvhash.c:319-328`

Impact here: filling eight people shards creates ~3 700 buckets, so ~3 700
directory rewrites of 4 KB ≈ **15 MB of flash writes for bookkeeping alone** —
comparable to the ~21 MB the actual data costs, and a large share of the
compaction load in B2.

Direction: the directory is a dense array of ids; a bucket's id could be derived
rather than stored, or the directory could be chunked so a bucket publish
rewrites 64 B instead of 4 KB.

### K6 — No iteration (major, `read`)

`map_ops` has no `iterate`. An application cannot enumerate, back up, migrate,
fsck or garbage-collect its own data. Every key must be regenerable from outside
the store — which this app arranges (F6) by making the whole dataset a pure
function of an index, but a real deployment with externally-sourced keys cannot.

`doc/layers/l3_interfaces.md` §3 records this as deferred until `kvtree`.

### K7 — Scratch buffers are file-scope and shared across all instances (minor, `read`)

`dir_buf` and `bkt_buf` (2 × `MAX_PAYLOAD` = 8 KB) are static and shared by
every `kvhash` instance in the image, reinforcing C7's single-thread contract at
a level where the caller may not expect it: two *different* `kvdb` handles are
not independent.

---

## L3 — `kvdb`

### V1 — `kvdb_has()` reports errors as "present" (moderate, `read`)

```c
int rc = db->ops->get(db->root, key, strlen(key), NULL, 0, NULL);
return rc != -ENOENT;
```

> `kvdb.c:213-221`

`-EIO` (flash error), `-EINVAL` and `-ENOMEM` all yield `true`. The header
states "true iff `kvdb_get` would find the key". A flash fault therefore reads
as an existing key — the wrong direction for anything making an access decision
on it.

Direction: return `rc == 0 || rc == -ENOMEM`, or give `kvdb_has` an `int`
return.

### V2 — A value's size cannot be learned cheaply (moderate, `read`)

`kvdb_get` sets `*out_len` even on `-ENOMEM`, so a size probe is possible — but
it costs the same two sector reads as the real get (B1). For variable-length
CBOR values an application must therefore either size a worst-case stack buffer
or pay double. This app does the former, and the buffer is 768 B on a stack that
`prj.conf` already has to enlarge for `blob_db`.

### V3 — No size, capacity or fill query on an instance (moderate, `read`)

Inherits B3 and K3. An application cannot ask how many entries an instance
holds, how close a shard is to K2's cliff, or when to add a shard.

### V4 — Nothing composes shards (moderate, `read`)

K1 makes sharding mandatory past ~2 MB, and the API offers no help: the
application invents instance naming, the key→shard hash, fan-out on read, and
per-shard capacity accounting. Every application that outgrows one instance will
reinvent the same layer.

> This is the finding most likely to justify an L3 addition rather than an L1/L2
> fix — a sharded-map interface over N `kvhash` instances.

---

## Not yet exercised

Findings that need the running app before they can be stated honestly:

- actual `-ENOSPC` rate from K2 at 10 000 persons (F11 counts them);
- whether the fill time matches the ≈ 2.5 h estimate, and how much is B2;
- compaction behaviour and latency spikes at ~50 % occupancy;
- whether CBOR encode/decode is measurable at all next to B1 (expected: no);
- `native_sim` versus DK divergence, which would be a finding about the
  `native_sim` model rather than the stack.
