# Design change proposal — spilling large keys and values out of `kvhash` buckets

Status: **implemented** · 2026-10-09 · what shipped, and where it differs from
this design: §9
· Target contract: `doc/layers/l2_containers.md` §4.2 (inlining) and §4.3
· Target implementation: `lib/containers/kvhash/kvhash.c`
· Evidence: `app_perf_kvdb/RESULTS.md`, "On values as blob ids"
· Governed by `doc/principles.md`

**The ask.** A kvhash bucket stores every key and value inline, so a large key
or value crowds the bucket it lands in (the `-ENOSPC` limit, FINDINGS.md K2),
and nothing larger than `ENTRY_LIMIT` can be stored at all. Storing *every*
value in its own blob was measured and trades too much read latency for small
values. The design kvlist planned (`l2_containers.md` §4.2) is the hybrid:
small keys and values inline, large ones in a blob of their own.

**The requirement this proposal is built around.** The two thresholds —
`KEY_INLINE_MAX` and `VAL_INLINE_MAX` — must be changeable at any time without
changing the on-flash format. After a change, new writes follow the new
thresholds, and every entry written under any earlier threshold stays readable,
writable and deletable.

**Short answer.** Every entry records how it is stored. Readers look only at
that record and never at a threshold; the thresholds are consulted in exactly
one place, when a `set` builds a new entry. A threshold is therefore a write
policy, not part of the format, and changing it is a rebuild, never a migration.

---

## 1. Entry format (kvhash v3)

The bucket stays a packed list. Each entry's length fields gain a flag bit:

```
[u16 klen | KSPILL] [u16 vlen | VSPILL] [key part] [value part]

key part   KSPILL = 0 : key bytes                      (klen bytes)
           KSPILL = 1 : [u32 key_fp] [u64 key_id]      (12 bytes)
value part VSPILL = 0 : value bytes                    (vlen bytes)
           VSPILL = 1 : [u64 val_id]                   (8 bytes)

KSPILL = bit 15 of klen, VSPILL = bit 15 of vlen; the low 15 bits are always
the TRUE key/value length, wherever the bytes live.
```

- **The flag bits are free in every existing bucket.** blob_db refuses at mount a
  payload cap above `(sector_size - 16) / 2 - 14` (`blob_db.c:495`), at most
  32 746 B, so an inline length can never reach 0x8000. A v2 bucket is therefore
  already a valid v3 bucket in which nothing is spilled.
- **The true length is kept even when spilled.** Lookups and enumeration order
  compare length first (§3), so most comparisons never need the spilled bytes.
- **`key_fp` is a fixed 32-bit hash of the key, independent of the bucket hash:
  CRC-32C (`crc32_c`, already in Zephyr).** It must not be derived from the
  CRC-32 that picks the bucket, because every key in a bucket shares 8 of those
  bits (4 for the sub-map, 4 for the bucket at fanout 16). It is part of the
  format: fixed forever in v3, never configurable.
- **A spilled key or value is one ordinary blob** holding the bytes and nothing
  else. With `CONFIG_BLOB_DB_LARGE_PAYLOADS` it may be larger than one slot,
  which lifts `ENTRY_LIMIT` for spilled parts entirely.
- **Validation.** A flagged entry must have exactly the size above (12 or 8
  bytes for the spilled part), and a spilled id must be non-zero; anything else
  is `-EIO`, as a truncated entry is today.

## 2. The rule that makes thresholds format-free

| Code path | Reads the flags | Reads the thresholds |
|---|:-:|:-:|
| `get`, `has`, `next`, `count` | yes | **never** |
| `del`, `destroy` | yes | **never** |
| `set` — finding the old entry | yes | **never** |
| `set` — building the new entry | — | **yes, only here** |

Consequences, each of which is a test (§6):

1. **Lowering a threshold** leaves existing inline entries above it inline.
   They are read as inline, because the flag says so. The next `set` of such a
   key writes it spilled.
2. **Raising a threshold** leaves existing spilled entries spilled. The next
   `set` writes them inline and frees their blobs after the commit.
3. **Migration is lazy and per key.** Nothing walks the map when a threshold
   changes; a key changes placement only when it is rewritten. A map may hold
   both placements of equal-sized entries side by side, forever.
4. **The thresholds are not stored in the map.** Storing them would make them
   format. They are Kconfig (`CONFIG_BLOB_CONTAINER_KVHASH_KEY_INLINE_MAX`,
   `..._VAL_INLINE_MAX`), read through two variables a test build can set at
   run time (§6).
5. **Any value is legal,** including 0 (spill everything) and the maximum (spill
   nothing, which is v2 behaviour). Spilling a key shorter than 12 B makes its
   entry bigger; that is allowed and merely pointless.

## 3. Lookup, set and order when both placements coexist

**Lookup** (`bkt_find`). For each entry: skip unless the true `klen` matches.
Then, if inline, compare bytes; if spilled, compare `key_fp` with the
requested key's CRC-32C and, only on a match, read `key_id` and compare bytes.
A hit on a spilled key costs one extra blob read; a miss costs one only on a
32-bit fingerprint collision between keys of equal length in one bucket. The
search must not stop at the first entry in the "expected" placement: a key
written under an old threshold may be stored the other way.

**Set.** Find the old entry as above. Build the new one under the *current*
thresholds. Then:

```
placement unchanged and only a spilled value changes:
    blob_db_update(val_id, new value)                 the whole mutation
otherwise:
    1. bind fresh blobs for any spilled part           (unreferenced so far)
    2. rewrite the bucket                              COMMIT
    3. delete the old entry's blobs no longer referenced
```

A spilled key whose placement does not change keeps its `key_id`; it is never
rewritten, since the key bytes did not change.

**Delete.** Rewrite the bucket without the entry (commit), then delete its
blobs.

**Enumeration order** (`order_cmp`, used by `next`) becomes
**(top index, sub index, true klen, key_fp, key bytes)** for *every* entry. For
an inline key the fingerprint is computed on the fly, which costs CPU and no
reads. Key bytes break a tie only on a full fingerprint collision, so a walk
reads a spilled key's blob only to return it. The order does not depend on
placement, so it does not change when a threshold does.

This changes the tie-break inside a bucket from v2's (klen, bytes). Order is
computed and never stored, so no data moves; the only observable difference is
that a walk started under v2 firmware and resumed under v3 may return keys in a
different order within one bucket. `shape_map.h` promises a stable order, not a
particular one; the release note should still say it.

## 4. Versioning, and old firmware

The directory `version` byte becomes 3. The thresholds stay out of it (§2.4).

- **v3 code, v3 map:** spilling enabled under the current thresholds.
- **v3 code, v2 map:** fully readable and writable, but never spilled. A v2
  directory tells old firmware it may parse the buckets, so v3 code must not
  put a flagged entry under one. It treats both thresholds as unlimited there.
  Upgrading such a map in place would need its directories rewritten, and the
  top directory is written only once by design; recreating the map is the
  supported path.
- **v2 code, v3 map:** refused by the existing version check (`dir_load`),
  which is exactly what protects old firmware from flagged entries.

So the format changes **once**, when spilling is introduced. Every threshold
change after that is a rebuild with nothing to migrate.

## 5. Crash consistency and leaks

The commit point stays a single blob write: the bucket rewrite, or the in-place
`blob_db_update` of a spilled value. A reader never sees a dangling id, because
blobs are bound before the bucket that references them and freed after it.

A crash between bind and commit, or between commit and free, leaves an
**unreferenced blob**: never a wrong answer, but leaked space. kvhash already has
this window for a fresh bucket ("reclaimed by a later format"). Spilling makes it
per-entry instead of per-bucket, so it should ship with a sweep: walk the map,
collect every referenced id, and delete the blobs blob_db holds that no map
references. That is open item O1; until it exists, the leak is bounded by one or
two blobs per interrupted mutation.

`destroy` must release every spilled blob of a bucket **before** that bucket's
own blob, so a crash mid-destroy still finds the references it needs. Today's
`release_buckets` deletes the bucket blob directly and must parse it first.
Already-deleted blobs return `-ENOENT`, which `release_buckets` already
tolerates, so a repeated destroy stays idempotent.

## 6. Tests

The requirement is only proven if one test writes under one threshold and reads
under another. Kconfig cannot change inside a test binary, so the thresholds are
read through two variables with a test-only setter
(`kvhash_test_set_inline_max(k, v)`, alongside `blob_db_test.h`).

1. Write N keys with mixed key and value sizes at thresholds (A, B); change to
   (A', B') covering lower, higher, 0 and unlimited; `get`, `walk` and `count`
   must return exactly the same content, in the same order.
2. After each change, rewrite half the keys: they move to the new placement;
   the other half keep theirs; everything still verifies. blob_db's live-blob
   count must equal map entries + structure + spilled parts (no leak).
3. `del` and `destroy` on a map holding both placements release every blob.
4. A v2 map, opened by v3 code with low thresholds: still inline after writes,
   still readable by a v2 reader's parser.
5. Fingerprint collision: two equal-length keys forced into one bucket with
   equal `key_fp` (test hook on the hash) must be told apart by their bytes.
6. Power loss at every step of §3's set and delete (the existing intent harness):
   every key wholly old or wholly new; unreferenced blobs at most as §5 states.

`app_perf_kvdb` keeps its `VALUE_BLOBS` variant as the all-spilled reference
point (`VAL_INLINE_MAX = 0`), and gains a run at the default threshold.

## 7. Defaults and expected effect

- **`VAL_INLINE_MAX = 64`**, the figure `l2_containers.md` §6 already names. At
  the benchmark's 24 B values nothing spills, and `app_perf_kvdb` must reproduce
  its inline counters exactly.
- **`KEY_INLINE_MAX = 32`.** A spilled key costs 12 B in the bucket and one extra
  read per hit; below about 32 B that buys nothing.
- **Credentials at ~61 B per entry spill nothing** under these defaults, so the
  capacity issue that started this (K2 at ~4 000 credentials) is untouched.
  Spilling bounds every entry's bucket footprint at 4 + 12 + 8 = 24 B, so it
  makes large records safe in a bucket. It does not remove the bucket limit;
  that remains the geometry fix or overflow chaining.

## 8. Open items

- **O1** Orphan sweep (§5): where it runs (mount, explicit call, or idle) and what
  it costs on a full store.
- **O2** Whether `map_config` should also accept per-map threshold overrides at
  create time. Allowed by §2, since nothing about them is persisted; useful for
  maps whose value sizes differ widely.
- **O3** DK measurement of a spilled `get` (the scaling in `app_perf_kvdb/RESULTS.md`
  predicts ≈1.33× per spilled hit) before the defaults are finalised.

## 9. As implemented

Built as designed in §1–§5, in `lib/containers/kvhash/kvhash.c`, with these
decisions made along the way:

- **Shipped defaults never spill** (both thresholds 32767), not the 64 / 32 of
  §7. At 64, `app_cbor_persondb`'s ~380 B person records would spill, adding a
  blob read to every person lookup — a performance change to an existing app
  that §7 did not intend, and that O3's DK measurement should decide.
  `app_perf_kvdb`'s inline counters are unchanged byte for byte at the default,
  and `app_cbor_persondb` verifies with no bucket overflows.
- **Run-time setter.** `kvhash_set_inline_max(key, val)` (`kvhash.h`) changes the
  policy for all maps, which is what the tests use to write under one policy
  and read under another in one binary. Per-map thresholds (O2) are not built.
- **Error codes.** A key or value of 0x8000–0xffff bytes is `-ENOSPC` (it cannot
  be stored, as before); a spilled key longer than one blob_db slot is
  `-ENOSPC`, since a lookup must be able to hold it. `get`/`del` of a key
  longer than 0x7fff is `-ENOENT`. Above 0xffff is still `-EINVAL`.
- **Key compares.** A lookup reads a spilled key whole into the free directory
  buffer (one blob read on a fingerprint match). The order's fingerprint-tie
  path streams both keys through the stack in 64 B pieces instead, because the
  walk is still using both buffers there.
- **`destroy` at depth 2** re-reads the top directory for each sub-map, since
  the bucket buffer now holds each bucket while its spilled blobs are released.
- **A build-time guard** asserts `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN <= 0x7fff`, the
  condition that keeps bit 15 free; blob_db cannot mount a larger cap anyway.

Tests (`tests/lib/containers/src/spill.c`, suite `kvhash_spill`) cover §6
items 1–5: policies lowered, raised, zeroed and disabled with no data moved and
the same walk order, lazy migration on rewrite, reuse of spilled parts, `del`,
remount, a v2 map, forced fingerprint collisions (including 150 B keys that
differ in the last byte, and inline and spilled keys of one length in one
bucket), and leak checks through `destroy`. **Not yet covered: item 6, power
loss inside set and delete** (open item O4), and spilled values larger than one
slot under `CONFIG_BLOB_DB_LARGE_PAYLOADS`.

`app_perf_kvdb` with `-DCONFIG_BLOB_CONTAINER_KVHASH_VAL_INLINE_MAX=0` (every
value spilled by kvhash itself) matches the app-level `VALUE_BLOBS` variant on
every read and write count, except `populate`: 26 788 reads against 46 982,
because a new key no longer pays a separate lookup before it is inserted. See
`app_perf_kvdb/RESULTS.md`.

- **O4** Power-loss tests for spilled set and delete, using the intent harness or
  blob_db's crash hooks.
